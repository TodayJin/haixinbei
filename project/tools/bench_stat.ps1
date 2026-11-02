# 台架 STAT 行 -> 稳态误差表
#
# 台架每跑完一个闭环窗口就打一行 STAT（见 firmware/App/Bsp/motor_bench.c）：
#   STAT tgt 1250 ff 0.79/1870 kp 20 ki 0.80/ 300 | L m ... p2p ... st ... o ... | R ... | outp ... n=...
# 这里把它解析成表，并算出每个目标点相对期望转速的误差百分比。
#
# 期望换算（台架自己的口径）：exp(计数/秒) = 目标 * 25 / 7200 * 200 = 目标 * 0.69444
#   —— SPEED_FULL_COUNTS=25（对应满速 4950 计数/秒 = PWM 7200），每 5ms 一拍 -> ×200。
#
# 判定线：|误差| <= 5% 打 OK，<= 8% 打 '~'，再差打 '!!'。
#
# 用法：pwsh -File tools\bench_stat.ps1 out\bench_ff3.log
param([Parameter(Mandatory = $true)][string]$Log)

$expK = 25.0 * 200.0 / 7200.0

function Verdict([double]$pct) {
    if ([math]::Abs($pct) -le 5.0) { return 'OK' }
    if ([math]::Abs($pct) -le 8.0) { return '~' }
    return '!!'
}

$rows = [System.Collections.Generic.List[object]]::new()

foreach ($hit in (Select-String -Path $Log -Pattern '^STAT ')) {
    $line = $hit.Line
    if ($line -notmatch 'tgt\s+(-?\d+)\s+ff\s+(\S+)\s+kp\s+(-?\d+)\s+ki\s+(\S+)') { continue }
    $tgt = [int]$Matches[1]; $ff = $Matches[2]; $kp = [int]$Matches[3]; $ki = $Matches[4]

    $m = [regex]::Matches($line, '[LR] m\s+(-?\d+) p2p\s+(-?\d+) st\s+(-?\d+) o\s+(-?\d+)')
    if ($m.Count -lt 2) { continue }

    $exp = [int]($tgt * $expK)
    $lm = [int]$m[0].Groups[1].Value; $lst = [int]$m[0].Groups[3].Value; $lo = [int]$m[0].Groups[4].Value
    $rm = [int]$m[1].Groups[1].Value; $rst = [int]$m[1].Groups[3].Value; $ro = [int]$m[1].Groups[4].Value
    $le = [math]::Round(($lm - $exp) * 100.0 / $exp, 1)
    $re = [math]::Round(($rm - $exp) * 100.0 / $exp, 1)

    $rows.Add([pscustomobject]@{
        tgt  = $tgt
        ff   = $ff
        kp   = $kp
        ki   = $ki
        exp  = $exp
        L_mean = $lm; L_err = $le; L_st = $lst; L_out = $lo
        R_mean = $rm; R_err = $re; R_st = $rst; R_out = $ro
        verdict = "$(Verdict $le)/$(Verdict $re)"
    })
}

if ($rows.Count -eq 0) { Write-Host "没有找到 STAT 行 —— 日志是不是没抓全？($Log)"; exit 1 }

$rows | Format-Table tgt, ff, kp, ki, exp, L_mean, L_err, L_st, L_out, R_mean, R_err, R_st, R_out, verdict -AutoSize

$bad = @($rows | Where-Object { $_.verdict -notmatch '^OK/OK$' })
Write-Host ("共 {0} 个窗口，其中 {1} 个未达 ±5%：" -f $rows.Count, $bad.Count)
$bad | ForEach-Object { Write-Host ("  tgt {0,5} ff {1,6} kp {2,3} ki {3,7}  L {4,6} ({5,5}%)  R {6,6} ({7,5}%)" -f $_.tgt, $_.ff, $_.kp, $_.ki, $_.L_mean, $_.L_err, $_.R_mean, $_.R_err) }
