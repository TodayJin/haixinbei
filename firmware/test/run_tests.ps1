# 主机侧单元测试（不需要连板）。
#   pwsh -File firmware/test/run_tests.ps1                # 全部
#   pwsh -File firmware/test/run_tests.ps1 -Only jy61p    # 只跑匹配名字的
#
# 做法：把固件里的**真源码**用 MinGW gcc 编成 PC 可执行文件，配套 firmware/test/stub
# 里的假 HAL。编译带 -fsanitize=undefined -fsanitize-undefined-trap-on-error：
# 数组越界/有符号溢出/除零/空指针/非法移位都会变成一条非法指令，进程直接死，
# 于是"随机输入不崩"就成了可自动判定的断言（MinGW 没有 asan/ubsan 运行库，
# 但 trap 模式不需要运行库）。
param(
  [string]$Only = ''
)
$ErrorActionPreference = 'Stop'
$testDir = $PSScriptRoot
$fw      = Split-Path -Parent $testDir
$stub    = Join-Path $testDir 'stub'
$cc      = 'C:\mingw64\bin\gcc.exe'
$outDir  = Join-Path $testDir 'build'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$incs = @(
  $stub,
  (Join-Path $fw 'Core/Inc'), (Join-Path $fw 'App/Inc'), (Join-Path $fw 'App/Bsp'),
  (Join-Path $fw 'App/Svc'), (Join-Path $fw 'App/Fonts'),
  (Join-Path $fw 'Middlewares/kk_oled/include'),
  (Join-Path $fw 'Middlewares/kk_ui/include'),
  (Join-Path $fw 'Middlewares/SEGGER_RTT')
) | ForEach-Object { "-I$_" }

$flags = @('-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter',
           '-fsanitize=undefined', '-fsanitize-undefined-trap-on-error')

# 每个测试要用到的"真源码"（路径相对 firmware/）
$deps = @{
  't_jy61p'    = @('App/Svc/jy61p.c')
  't_pid'      = @('App/Svc/pid.c')
  't_tracing'  = @('App/Svc/tracing.c')
  # host_link.c 只调 HostExt_*（真业务在 host_ext.c），所以协议层测试用 host_extstub.c 顶替
  't_hostlink' = @('App/Svc/host_link.c', 'App/Svc/jy61p.c', 'test/stub/host_extstub.c')
  # 反过来，host_ext.c 的控制模块依赖用 host_appstub.c 顶替
  't_hostext'  = @('App/Svc/host_ext.c', 'test/stub/host_appstub.c')
  't_tracecal' = @('App/Svc/trace_cal.c', 'App/Svc/tracing.c')
't_speedctrl' = @('App/Svc/speed_control.c')
't_gyrotest'  = @('App/Svc/gyro_control.c')
't_motion'    = @('App/Svc/motion.c', 'App/Svc/jy61p.c')
't_bushealth' = @('App/Svc/bus_health.c')
}
$stubs = @('host_hal.c', 'host_board.c', 'host_rtt.c') | ForEach-Object { Join-Path $stub $_ }

$tests = Get-ChildItem -Path $testDir -Filter 't_*.c' -File | Sort-Object Name
if ($Only) { $tests = $tests | Where-Object { $_.Name -match $Only } }

$pass = 0; $fail = 0; $buildFail = 0
foreach ($t in $tests) {
  $name = [System.IO.Path]::GetFileNameWithoutExtension($t.Name)
  $exe  = Join-Path $outDir "$name.exe"
  $srcs = @($t.FullName)
  if ($deps.ContainsKey($name)) { $srcs += ($deps[$name] | ForEach-Object { Join-Path $fw $_ }) }
  $srcs += $stubs

  Write-Host "== $name" -ForegroundColor Cyan
  $log = & $cc @flags @incs @srcs '-I' $testDir -o $exe 2>&1
  $bad = $log | Where-Object { $_ -match 'error:|warning:' }
  if ($LASTEXITCODE -ne 0) {
    $log | Select-Object -Last 30 | ForEach-Object { Write-Host "   $_" -ForegroundColor Yellow }
    Write-Host "   编译失败" -ForegroundColor Red
    $buildFail++
    continue
  }
  if ($bad) { $bad | Select-Object -First 10 | ForEach-Object { Write-Host "   [warn] $_" -ForegroundColor DarkYellow } }

  $run = & $exe 2>&1
  $code = $LASTEXITCODE
  $run | ForEach-Object { Write-Host "   $_" }
  # 退出码为 0 还不够：测试若忘了 `return T_SUMMARY();`，失败数就丢在输出里、
  # main 仍返回 0（2026-10-02 真踩过：t_gyrotest 报了 3 处失败却判"通过"）。
  # 所以再核一遍输出里必须是"0 处失败"。
  $zeroFail = [bool]($run | Select-String -SimpleMatch '0 处失败' -Quiet)
  if ($code -eq 0 -and -not $zeroFail) {
    $fail++
    Write-Host "   失败（退出码 0 但输出里不是 0 处失败 —— 检查该测试是否漏了 return T_SUMMARY()）" -ForegroundColor Red
  } elseif ($code -eq 0) {
    $pass++
    Write-Host "   通过" -ForegroundColor Green
  } else {
    $fail++
    $hex = ('0x{0:X8}' -f ($code -band 0xFFFFFFFF))
    Write-Host "   失败（退出码 $code / $hex）" -ForegroundColor Red
    if ($code -eq -1073741795) { Write-Host "   ↑ 0xC000001D = UBSan trap：越界/溢出/除零/空指针/非法移位" -ForegroundColor Red }
  }
}

Write-Host ""
Write-Host ("主机测试：通过 {0}，失败 {1}，编译失败 {2}（共 {3}）" -f $pass, $fail, $buildFail, $tests.Count)
if ($fail -gt 0 -or $buildFail -gt 0) { exit 1 }
