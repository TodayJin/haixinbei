# 离线静态检查（不需要连板）：GCC -fanalyzer + -Wextra 逐文件编译我们的源码。
#   pwsh -File tools/check_static.ps1                 # 全部
#   pwsh -File tools/check_static.ps1 -Only app_ui    # 只查文件名含 app_ui 的
# 输出：每个文件一段，只打印有告警的行；末尾汇总。退出码 = 有 error 的为 1。
param(
  [string]$Only = '',
  [switch]$Wextra
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fw = Join-Path $root 'firmware'
$cube = 'C:/Users/Today/STM32Cube/Repository/STM32Cube_FW_G4_V1.6.3'
$gcc = 'C:/arm-gnu-toolchain/bin/arm-none-eabi-gcc.exe'

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
