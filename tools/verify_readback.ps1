<#
 回读逐字节比对：把 bin 和从芯片 savebin 出来的文件比。
 用法（仓库根目录）：
   pwsh -File tools\verify_readback.ps1 -Bin <bin> -Readback <readback.bin>
 一致 -> 退出码 0；不一致 -> 打印 firstDiff 并以退出码 1 结束。

 必须容忍的已知差异（不容忍的话每次都会假失败）：
   0x080001D8..0x080001DF 这 8 字节是 .isr_vector 与 .text 之间的对齐空洞。
   objcopy -O binary 用 0x00 填这段，ihex 里没有它、芯片 Flash 保持擦除态 0xFF。
   2026-09-30 实测：bin=0x00 / Flash=0xFF，正好 8 字节。
#>
param(
    [Parameter(Mandatory=$true)][string]$Bin,
    [Parameter(Mandatory=$true)][string]$Readback
)
$ErrorActionPreference = "Stop"
$holeStart = 0x1D8
$holeLen   = 8
if (-not (Test-Path $Bin))      { throw "找不到 bin：$Bin" }
if (-not (Test-Path $Readback)) { throw "找不到回读文件：$Readback（savebin 那一步没成功？）" }
$a = [IO.File]::ReadAllBytes($Bin)
$b = [IO.File]::ReadAllBytes($Readback)
if ($b.Length -lt $a.Length) { throw "回读只有 $($b.Length) 字节，bin 是 $($a.Length) 字节（镜像被截断了？）" }
$bad = 0; $firstDiff = -1; $hole = 0
for ($i = 0; $i -lt $a.Length; $i++) {
    if ($a[$i] -eq $b[$i]) { continue }
    if ($i -ge $holeStart -and $i -lt ($holeStart + $holeLen) -and $a[$i] -eq 0 -and $b[$i] -eq 255) { $hole++; continue }
    if ($firstDiff -lt 0) { $firstDiff = $i }
    $bad++
}
if ($bad -ne 0) {
    Write-Host ("FAIL：回读不一致 firstDiff=0x{0:X}，不一致 {1} 字节（bin {2} 字节）" -f $firstDiff, $bad, $a.Length)
    exit 1
}
Write-Host ("OK：回读一致 {0} 字节，firstDiff=none（其中 0x{1:X}..0x{2:X} 共 {3} 字节是已知对齐空洞）" -f $a.Length, $holeStart, ($holeStart + $holeLen - 1), $hole)
exit 0
