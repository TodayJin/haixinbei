# 离线静态检查（不需要连板）：GCC -fanalyzer + -Wextra 逐文件编译我们的源码。
#   pwsh -File tools/check_static.ps1                 # 全部
#   pwsh -File tools/check_static.ps1 -Only app_ui    # 只查文件名含 app_ui 的
# 输出：每个文件一段，只打印有告警的行；末尾汇总。退出码 = 有 error 的为 1。
param(
  [string]$Only = '',
  [switch]$Wextra,
  [string]$Cube = '',        # STM32Cube G4 包根目录（空 = 自动找）
  [string]$CC   = ''         # arm-none-eabi-gcc（空 = 自动找）
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fw = Join-Path $root 'firmware'

# ---- ★2026-10-14（审计 #30）：路径解析三级回落 ----
#   原来这里是**别的机器**的硬编码路径（C:/Users/Today/STM32Cube/... 与
#   C:/arm-gnu-toolchain/bin/...），而本脚本 $ErrorActionPreference='Stop' ⇒
#   在这台机器上它在第一个文件就终止（报"找不到编译器"），于是**"严格告警"这条路
#   在环境迁移到 _tools/ 之后从来没真正跑过一次**，而文档还在记录它"通过"。
#   现在：显式参数 → 环境变量 → 自动查找（含仓库内 _tools 那份）→ 旧硬编码；都没有就
#   带**可行动的提示**退出（而不是抛个 CommandNotFoundException 让人猜）。
function Resolve-CubeG4([string]$explicit) {
    $cands = @()
    if ($explicit)         { $cands += $explicit }
    if ($env:STM32CUBE_G4) { $cands += $env:STM32CUBE_G4 }
    # ★逐级向上找（仓库布局可能不同）：tools/ 往上两级才是与 _tools 平级的那层
    $d = $PSScriptRoot
    for ($i = 0; $i -lt 4; $i++) {
        $d = Split-Path -Parent $d
        if (-not $d) { break }
        $cands += (Join-Path $d '_tools\STM32Cube_FW_G4_V1.6.3')
    }
    $cands += 'C:/Users/Today/STM32Cube/Repository/STM32Cube_FW_G4_V1.6.3'
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'Drivers\STM32G4xx_HAL_Driver\Inc\stm32g4xx_hal.h'))) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    return ''
}
function Resolve-ArmGcc([string]$explicit) {
    if ($explicit -and (Test-Path -LiteralPath $explicit)) { return $explicit }
    if ($env:CC -and (Test-Path -LiteralPath $env:CC))     { return $env:CC }
    $g = Get-Command 'arm-none-eabi-gcc' -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    if (Test-Path -LiteralPath 'C:/arm-gnu-toolchain/bin/arm-none-eabi-gcc.exe') {
        return 'C:/arm-gnu-toolchain/bin/arm-none-eabi-gcc.exe'
    }
    return ''
}
$cube = Resolve-CubeG4 $Cube
if (-not $cube) {
  Write-Host '找不到 STM32Cube G4 包：用 -Cube <路径> / 环境变量 STM32CUBE_G4 / 放到 <仓库>/_tools/STM32Cube_FW_G4_V1.6.3' -ForegroundColor Red
  exit 2
}
$gcc = Resolve-ArmGcc $CC
if (-not $gcc) {
  Write-Host '找不到 arm-none-eabi-gcc：装工具链并加进 PATH，或用 -CC <路径> / 环境变量 CC' -ForegroundColor Red
  exit 2
}
Write-Host ("[环境] cube = " + $cube)
Write-Host ("[环境] gcc  = " + $gcc)

$incs = @(
  'Core/Inc', 'App/Inc', 'App/Bsp', 'App/Svc', 'App/Fonts',
  'Middlewares/SEGGER_RTT', 'Middlewares/kk_oled/include',
  'Middlewares/kk_oled/graphics', 'Middlewares/kk_oled/driver',
  'Middlewares/kk_ui/include'
) | ForEach-Object { '-I' + (Join-Path $fw $_) }
$incs += @(
  "-I$cube/Drivers/STM32G4xx_HAL_Driver/Inc",
  "-I$cube/Drivers/STM32G4xx_HAL_Driver/Inc/Legacy",
  "-I$cube/Drivers/CMSIS/Device/ST/STM32G4xx/Include",
  "-I$cube/Drivers/CMSIS/Include"
)
$base = @('-mcpu=cortex-m4','-mthumb','-mfpu=fpv4-sp-d16','-mfloat-abi=hard',
          '-DUSE_HAL_DRIVER','-DSTM32G474xx','-Og','-g3','-Wall','-Wno-unused-parameter',
          '-fdata-sections','-ffunction-sections')
# 注意：不能用 -fsyntax-only —— 那样 -fanalyzer 根本不会跑（分析在代码生成阶段做）。
$obj = Join-Path $env:TEMP '_an.o'

$dirs = @('Core/Src','Core/Inc','App/Src','App/Bsp','App/Svc','App/Inc','App/Fonts',
          'Middlewares/kk_ui/src','Middlewares/kk_oled/graphics','Middlewares/kk_oled/driver')
$files = @()
foreach ($d in $dirs) {
  $p = Join-Path $fw $d
  if (Test-Path $p) { $files += Get-ChildItem -Path $p -Filter *.c -File }
}
$files = $files | Where-Object { $_.FullName -notmatch '\\build' }
if ($Only) { $files = $files | Where-Object { $_.Name -match $Only } }

$errs = 0
$warned = 0
foreach ($f in $files) {
  $extra = @('-fanalyzer')
  if ($Wextra) { $extra += '-Wextra' }
  $o = & $gcc @base @extra @incs $f.FullName -c -o $obj 2>&1
  $lines = $o | Where-Object { $_ -match 'warning:|error:' }
  if ($lines) {
    $rel = $f.FullName.Substring($root.Length + 1)
    Write-Host "==== $rel" -ForegroundColor Cyan
    $lines | ForEach-Object { Write-Host "   $_" }
    if ($lines | Where-Object { $_ -match 'error:' }) { $errs++ } else { $warned++ }
  }
}
Write-Host ""
Write-Host ("扫描 {0} 个文件：{1} 个有告警，{2} 个有 error" -f $files.Count, $warned, $errs)
if ($errs -gt 0) { exit 1 }
