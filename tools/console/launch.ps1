# 上位机桥的一键启动器（真车 / 仿真）。
#
# 同目录的三个 .bat 只是双击入口（**保持纯 ASCII**，中文提示一律从这里出：
# chcp 65001 和同一个 .bat 里的非 ASCII 字节会让 cmd 按错位重新读文件、把注释片段当命令跑）；
#   启动上位机.bat        —— 真车：自动找串口，921600（= 固件 firmware/Core/Src/board.c 的 DL_BAUD）
#   启动上位机-仿真.bat   —— 内置仿真车，没插车也能开界面
#   启动上位机-列串口.bat —— 只列串口（诊断：车插上没、是哪个 COM）
# 仓库根还有一个同名的 启动上位机.bat 转发到这里，可以直接带开关：-Sim / -ListPorts / -HttpPort 8791。
#
# 手工用：pwsh -File tools\console\launch.ps1 -Sim -NoBrowser
# 额外给桥塞参数：launch.ps1 -Extra '--sse-batch-ms','0'
[CmdletBinding()]
param(
    [switch]$Sim,                                   # 用内置仿真车
    [switch]$ListPorts,                             # 只列串口后退出
    [string]$Port = "",                             # 串口号，例如 COM26；空 = 自动找
    [int]$Baud = 921600,                            # 和板上固件一致；车上第二颗探针的 VCOM 实测能到 3 Mbaud
    [int]$HttpPort = 8770,                          # 浏览器访问的端口（= 桥的 --port，不是串口！）
    [switch]$NoBrowser,                             # 不自动开浏览器
    [switch]$Pause,                                 # 结束后停一下（双击进来的默认行为）
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Extra = @()
)

$ErrorActionPreference = 'Stop'
# 本脚本会故意跑一些非零退出的命令（探测 Python、试开串口）：PowerShell 7.4+ 上
# 别让「原生命令非零退出」升级成终止错误。
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch { }
try { $OutputEncoding = [Console]::OutputEncoding } catch { }

$ConsoleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ConsoleDir

function Say([string]$m) { Write-Host $m }
function Fail([string]$m, [int]$code = 1) {
    Write-Host ""
    Write-Host "启动失败：$m" -ForegroundColor Red
    exit $code
}

# ---- 1. 找 Python（3.11+ 标准库 + pyserial，桥只依赖这些）----
$Py = $null
$PyVer = ""
foreach ($cand in @(@{ e = 'py'; a = @('-3') }, @{ e = 'python'; a = @() }, @{ e = 'python3'; a = @() })) {
    if (-not (Get-Command $cand.e -ErrorAction SilentlyContinue)) { continue }
    $v = & $cand.e @($cand.a + @('-c', 'import sys;print(sys.version.split()[0])')) 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $v) { continue }
    $sv = & $cand.e @($cand.a + @('-c', 'import serial;print(serial.__version__)')) 2>$null
    if ($LASTEXITCODE -ne 0) {
        Fail "找到了 $($cand.e)（Python $v），但缺 pyserial。先跑：`n    $($cand.e) $($cand.a -join ' ') -m pip install pyserial"
    }
    $Py = $cand
    $PyVer = "Python $v + pyserial $sv"
    break
}
if (-not $Py) {
    Fail "没找到 Python（试过 py -3 / python / python3）。装一个 3.11 以上再回来：`n    winget install Python.Python.3.12`n装完还要：`n    python -m pip install pyserial"
}

# ---- 2. 只列串口：诊断用，不动桥 ----
if ($ListPorts) {
    Say "Python：$PyVer"
    Say ""
    & $Py.e @($Py.a + @('-m', 'bridge.main', '--list-ports'))
    $rc = $LASTEXITCODE
    if ($Pause) { Say ""; [void](Read-Host "按回车关闭窗口") }
    exit $rc
}

# ---- 3. 前端产物 + HTTP 端口占用预检 ----
$indexHtml = Join-Path $ConsoleDir 'web\dist\index.html'
if (-not (Test-Path $indexHtml)) {
    Say "注意：前端还没构建（找不到 web\dist\index.html），界面会是白的。先跑："
    Say "    cd `"$ConsoleDir\web`"; npm install; npm run build"
    Say ""
}

function Test-HttpPortBusy([int]$p) {
    try { return [bool](Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction Stop) } catch { }
    try { return [bool](netstat -ano | Select-String -Pattern (":$p\s+.*LISTENING")) } catch { }
    return $false
}
if (Test-HttpPortBusy $HttpPort) {
    $holder = (netstat -ano | Select-String -Pattern (":$HttpPort\s+.*LISTENING") | Select-Object -First 1)
    $hint = "换一个端口：launch.ps1 -HttpPort 8771"
    if ($holder -and $holder.Line -match 'LISTENING\s+(\d+)') {
        $hint = "占用者 PID $($Matches[1])：Stop-Process -Id $($Matches[1])　或换端口：-HttpPort 8771"
    }
    Fail "HTTP 端口 $HttpPort 已经被占了（多半是上一次的桥没关干净）。`n    $hint" 3
}

# ---- 4. 选链路：真车先找串口 ----
if (-not $Sim) {
    Say "正在找串口……"
    $listed = & $Py.e @($Py.a + @('-m', 'bridge.main', '--list-ports')) 2>&1
    foreach ($line in $listed) { Say "  $line" }
    if ($Port -eq "") {
        $m = ($listed | Select-String -Pattern '--serial\s+(\S+)' | Select-Object -First 1)
        if ($m) { $Port = $m.Matches[0].Groups[1].Value }
    }
    if ($Port -eq "") {
        Fail "没找到串口。车插上（USB 线 + 车上电）再试；没有车就双击『启动上位机-仿真.bat』。"
    }
    Say ""
    Say "用串口：$Port @$Baud"

    # 先试着开一下这个串口：被别人占着 / 线松了，这里就能先说人话（不拦，桥自己也会报，
    # 但那个报错要等浏览器起来才看得见）。
    $probe = & $Py.e @($Py.a + @('-c',
        'import sys,serial;s=serial.Serial(sys.argv[1],int(sys.argv[2]),timeout=0.2);s.close()',
        $Port, "$Baud")) 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "注意：$Port 现在打不开 ——" -ForegroundColor Yellow
        Write-Host "    $($probe | Select-Object -Last 1)" -ForegroundColor Yellow
        Write-Host "    常见原因：串口助手 / 上一次的桥还占着它（关掉再来），或者 USB 线/车电。" -ForegroundColor Yellow
        Say ""
    }
}

# ---- 5. 起桥（前台跑，Ctrl+C 或关窗口即停）----
$bridgeArgs = @('-m', 'bridge.main', '--port', "$HttpPort", '--baud', "$Baud")
if ($Sim) { $bridgeArgs += '--sim' } else { $bridgeArgs += @('--serial', $Port) }
if ($NoBrowser) { $bridgeArgs += '--no-browser' }

# ---- ★2026-10-11：管理员权限下**不要**让桥去开浏览器 ----
#  提升权限的进程去开 Edge/Chrome 时，Windows 会弹这么一句：
#    「Microsoft Edge 未响应，因为现有实例正在以提升的权限运行。是否要用普通权限重启现有实例？」
#  弹窗本身无害（点「是」就行），但每次启动都吓人一次 —— 用户实测每次都弹。
#  根因是「上位机管理员启、浏览器普通权限」，跨权限级别开浏览器必然如此，脚本里绕不过去
#  （除非连浏览器一起提权，那更糟）。所以干脆不开，只把网址打出来让人自己点。
#  正常双击「启动上位机.bat」是**不会**提权的；会提权一般是：右键「以管理员身份运行」、
#  或者从一个「管理员：PowerShell / 终端」窗口里启动的。
$isAdmin = $false
try {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { $isAdmin = $false }
$noOpen = $NoBrowser
if ($isAdmin -and -not $NoBrowser) {
    $bridgeArgs += '--no-browser'
    $noOpen = $true
    Write-Host ""
    Write-Host "★ 这个窗口是【管理员权限】—— 已经不让桥自动开浏览器了。" -ForegroundColor Yellow
    Write-Host "  原因是提升权限的进程去开 Edge 会弹「现有实例正在以提升的权限运行」那个框。" -ForegroundColor Yellow
    Write-Host "  下面会把网址打出来，你自己点一下就行（功能完全不受影响）。" -ForegroundColor Yellow
    Write-Host "  想恢复自动开浏览器：别用管理员，正常双击「启动上位机.bat」即可。" -ForegroundColor Yellow
    Write-Host ""
}
if ($Extra.Count -gt 0) { $bridgeArgs += $Extra }

Say "工作目录：$ConsoleDir"
if ($noOpen) {
    Say "界面：http://127.0.0.1:$HttpPort/　（★没有自动开浏览器，请手动打开这个网址；停：Ctrl+C 或关这个窗口）"
} else {
    Say "界面：http://127.0.0.1:$HttpPort/　（桥自己会开浏览器；停：Ctrl+C 或关这个窗口）"
}
Say ""

& $Py.e @($Py.a + $bridgeArgs)
$rc = $LASTEXITCODE

if ($rc -ne 0) {
    Write-Host ""
    Write-Host "桥退出了，退出码 $rc（上面那几行是原因）。常用排查：" -ForegroundColor Yellow
    Write-Host "    启动上位机-列串口.bat   看车在哪个 COM"
    Write-Host "    串口被别的程序占着（串口助手 / 另一个桥）就关掉它"
    Write-Host "    只调界面：启动上位机-仿真.bat"
}
if ($Pause) { Say ""; [void](Read-Host "按回车关闭窗口") }
exit $rc
