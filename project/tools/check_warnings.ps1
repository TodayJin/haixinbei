# ============================================================
#  额外编译告警巡检（离线，不接板）
#
#  用比正式构建更严的告警集把**主程序构建实际用到的每一个 C 文件**编一遍，
#  把告警按“框架必需（白名单）”和“需要处理”分开，后者非空就退出码 1。
#
#  用法：
#     pwsh -File tools/check_warnings.ps1                 # 打印分类结果
#     pwsh -File tools/check_warnings.ps1 -Out out/warn.txt
#     pwsh -File tools/check_warnings.ps1 -List           # 连白名单项也逐条列出
#
#  为什么需要它：正式构建只用 -Wall，像 `int abs(int)` 这种**顶掉标准库符号**的
#  问题它不报（2026-10-03 就是这么揪出来的：motor.c 里一个没人调用的全局 abs()
#  把 newlib 的 abs 顶掉了）。extra 告警集换成：
#     -Wextra -Wswitch-enum -Wswitch-default -Wshadow -Wcast-align
#     -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wwrite-strings
#
#  注意：编译产物必须真的生成（脚本会检查 .o 是否存在）。早先版本用
#  `Get-ChildItem -Name` 拿到的是纯文件名，编译全部失败但错误行不含 "warning:"
#  就被过滤掉了 —— 于是“0 告警”，等于什么都没查。
# ============================================================
[CmdletBinding()]
param(
    [string]$Firmware = (Join-Path $PSScriptRoot '..\firmware'),
    [string]$Out = '',
    [switch]$List,
    [string]$Cube = '',        # STM32Cube G4 包根目录（空 = 自动找）
    [string]$CC   = ''         # arm-none-eabi-gcc（空 = 自动找）
)

$ErrorActionPreference = 'Stop'

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
    Write-Host '找不到 STM32Cube G4 包。用 -Cube <路径> 指定，或设环境变量 STM32CUBE_G4，' -ForegroundColor Red
    Write-Host '或把包放在 <仓库>/_tools/STM32Cube_FW_G4_V1.6.3（推荐）。' -ForegroundColor Red
    exit 2
}
$cc = Resolve-ArmGcc $CC
if (-not $cc) {
    Write-Host '找不到 arm-none-eabi-gcc。装 GNU Arm Embedded 工具链并加进 PATH，' -ForegroundColor Red
    Write-Host '或用 -CC <路径> 指定，或设环境变量 CC。' -ForegroundColor Red
    exit 2
}
Write-Host ("[环境] cube = " + $cube)
Write-Host ("[环境] gcc  = " + $cc)

$hal = "$cube/Drivers/STM32G4xx_HAL_Driver"
$cmsisd = "$cube/Drivers/CMSIS/Device/ST/STM32G4xx"
$cmsis = "$cube/Drivers/CMSIS"

$inc = @('-ICore/Inc','-IApp/Inc','-IApp/Bsp','-IApp/Svc','-IApp/Fonts',
         '-IMiddlewares/SEGGER_RTT','-IMiddlewares/kk_oled/include',
         '-IMiddlewares/kk_oled/graphics','-IMiddlewares/kk_oled/driver',
         '-IMiddlewares/kk_ui/include',
         "-I$hal/Inc","-I$hal/Inc/Legacy","-I$cmsisd/Include","-I$cmsis/Include")

$extra = @('-Wextra','-Wswitch-enum','-Wswitch-default','-Wshadow','-Wcast-align',
           '-Wpointer-arith','-Wstrict-prototypes','-Wmissing-prototypes','-Wundef',
           '-Wwrite-strings')

# 主程序构建（make，无 SELFTEST）实际编的文件：
#   CORE_COMMON + Core/Src/main_app.c + App/** + kk_ui + kk_oled
# HAL/CMSIS 是第三方代码，不进巡检。
$patterns = @(
    'Core/Src/board.c','Core/Src/stm32g4xx_it.c','Core/Src/syscalls.c','Core/Src/main_app.c',
    'App/Src/*.c','App/Bsp/*.c','App/Svc/*.c','App/Fonts/*.c',
    'Middlewares/kk_ui/src/*.c','Middlewares/kk_oled/graphics/*.c'
)

# 白名单：框架要求这么写，改不了 / 改了没意义。
#   键 = 源文件（相对 firmware/），值 = 允许出现的告警片段数组
$baseline = @{
    'Core/Src/stm32g4xx_it.c' = @('no previous prototype for')   # 中断向量表要求全局符号
    'Core/Src/syscalls.c'     = @('no previous prototype for')   # newlib 的 _write/_sbrk 等弱符号桩
    'App/Src/app_ui.c'        = @("enumeration value 'TRACE_CAL_IDLE' not handled in switch")
    'Middlewares/kk_ui/src/kk_ui.c' = @("enumeration value 'KK_UI_DEFER_NONE' not handled in switch")
}

function Test-Baselined([string]$file, [string]$msg) {
    $key = ($file -replace '\\','/')
    if (-not $baseline.ContainsKey($key)) { return $false }
    foreach ($frag in $baseline[$key]) { if ($msg -like "*$frag*") { return $true } }
    return $false
}

Push-Location $Firmware
try {
    $files = @()
    foreach ($p in $patterns) { $files += @(Get-ChildItem $p -ErrorAction SilentlyContinue) }
    $files = $files | Sort-Object FullName -Unique

    $results = New-Object System.Collections.Generic.List[object]
    $compileFailed = New-Object System.Collections.Generic.List[string]

    foreach ($fi in $files) {
        $rel = (Resolve-Path -Relative $fi.FullName) -replace '^\.\\',''
        $obj = Join-Path $env:TEMP '_warnchk.o'
        if (Test-Path $obj) { Remove-Item $obj -Force }
        $log = & $cc -c -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard `
                     -DUSE_HAL_DRIVER -DSTM32G474xx @inc -Og -g3 -Wall -Wno-unused-parameter `
                     @extra -fdata-sections -ffunction-sections $rel -o $obj 2>&1
        if (-not (Test-Path $obj)) {
            $compileFailed.Add("$rel :: " + (($log | Select-Object -First 3) -join ' / '))
            continue
        }
        foreach ($line in $log) {
            if ($line -notmatch 'warning:') { continue }
            if ($line -match '^\s*([A-Za-z]:/[^ ]*STM32Cube[^ ]*|\S+\.h:\d+)') { continue }
            $m = [regex]::Match($line, '^([^:]+):(\d+):\d+: warning: (.*)$')
            if (-not $m.Success) { continue }
            $src = $m.Groups[1].Value -replace '\\','/'
            $results.Add([pscustomobject]@{
                File    = $src
                Line    = [int]$m.Groups[2].Value
                Message = $m.Groups[3].Value
                Waived  = (Test-Baselined $src $m.Groups[3].Value)
            })
        }
    }

    $waived = @($results | Where-Object { $_.Waived })
    $todo   = @($results | Where-Object { -not $_.Waived })

    $sb = New-Object System.Text.StringBuilder
    $say = { param($s) [void]$sb.AppendLine($s); Write-Host $s }

    & $say ("额外告警巡检：编译 " + $files.Count + " 个文件（白名单 " + $waived.Count + " 条，待处理 " + $todo.Count + " 条）")
    if ($compileFailed.Count -gt 0) {
        & $say "编译失败："
        foreach ($c in $compileFailed) { & $say ("  !! " + $c) }
    }
    if ($todo.Count -gt 0) {
        & $say "待处理告警："
        foreach ($w in ($todo | Sort-Object File, Line)) {
            & $say ("  " + $w.File + ":" + $w.Line + "  " + $w.Message)
        }
    } else {
        & $say "待处理告警：无"
    }
    if ($List -and $waived.Count -gt 0) {
        & $say "白名单（框架必需）："
        foreach ($w in ($waived | Sort-Object File, Line)) {
            & $say ("  " + $w.File + ":" + $w.Line + "  " + $w.Message)
        }
    }

    if ($Out) {
        $dir = Split-Path -Parent $Out
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        Set-Content -Path $Out -Value $sb.ToString() -Encoding UTF8
        Write-Host "已写入 $Out"
    }

    if ($compileFailed.Count -gt 0 -or $todo.Count -gt 0) { exit 1 }
    exit 0
}
finally { Pop-Location }
