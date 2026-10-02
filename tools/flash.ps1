<#
 烧录 haixin-app.bin 并**强制回读逐字节比对**（回读是唯一可信的结论）。
 用法（仓库根目录）：
   pwsh -File tools\flash.ps1                                   # 用默认 bin
   pwsh -File tools\flash.ps1 -Bin <path> -Device STM32G474RE   # 指定 bin / 器件
 make 里也是调它：make flash

 擦除范围（2026-09-30 用户 m03397 要求「别再把设置擦了」）：
   **默认不整片擦**：CubeProgrammer 下载自带"按镜像范围擦扇区"（--skipErase 才是关掉它），
   范围就是 app 的扇区 0..N；Flash 最后一页 = 扇区 63 = 0x0801F800..0x0801FFFF
   （nvram.c 在那儿存菜单设置和黑白标定，链接脚本也只给 app 126K）**不碰**，
   所以烧完菜单设置/标定还在。真要一片干净：pwsh -File tools\flash.ps1 -MassErase

 两条路径：
   1) 有 STM32CubeProgrammer CLI（$cp）-> 编程 + 校验（擦除范围见上），然后照样做 J-Link savebin 回读。
   2) 没有（没装 CLT 的机器）-> 直接 J-Link loadbin（它本身也只擦写到的扇区），然后 savebin 回读。
     （本机 2026-09-30 已装 D:\APP\ST\STM32CubeCLT_1.22.0，CLI 2.23.0 可用；环境变量
       STM32_PROGRAMMER_CLI 与用户 PATH 都指过去了，探测是瞬时的。）
 只有回读逐字节一致才算成功（退出码 0）；不一致自动重试（默认 2 次），全失败退出码 1。

 为什么只有回读可信（2026-09-29 / 09-30 实测，J-Link DLL 6.82c + 这颗 G474）：
   * 命令行 -device STM32G474RB 会**静默假成功**：打印 O.K. 却一个字节没写（FLASH_SR 停在 0xA0）。
     用 -device STM32G474RE 才对，别改回 RB。
   * 整片 erase 之后的第一次 loadbin 一定失败（"Error while programming flash: Programming failed."），
     所以 loadbin 之前先复位一次。
   * 只差一小段时（J-Link compare 判定"只需写 1 个 2048 字节范围"），它会报
     "Programming failed @ 0x08010534 (block verification error) / Failed to restore target"，
     而 savebin 回读证明内容其实是对的 —— 它自己的结论（O.K. 也好 failed 也好）两个方向都不能信。
   * 内容已经一样时它**什么都不写**却照样报成功：CubeProgrammer 打印 "Download verified successfully"、
     J-Link 打印 "Flash download: Bank 0 @ 0x08000000: Skipped. Contents already match"。
     所以「成功」不能证明擦/写在这颗芯片上真的可行 —— 只有回读逐字节比对能证明内容对。
     擦/写真出问题时 FLASH->SR(0x40022010) 会粘着错误标志（PGAERR|PGSERR…），脚本现在会把它读出来警告。
   * J-Link Commander 会在 DEMCR(0xE000EDFC) 留 VC_CORERESET(0x01000000)：调试器一撤，
     下一次复位会被当调试事件升级成 HardFault。所以复位前后各清一次。
   * 回读比对容忍 0x080001D8..0x080001DF 的 8 字节 0x00/0xFF 空洞（只有在用 .hex 编程时才出现；
     用 loadbin 会把这段也写进去，通常是 0 字节）。
#>
param(
    [string]$Bin    = (Join-Path (Split-Path $PSScriptRoot -Parent) 'firmware\build-app\haixin-app.bin'),
    [string]$JLink  = 'C:\Program Files (x86)\SEGGER\JLink\JLink.exe',
    [string]$Device = 'STM32G474RE',
    [int]$Retries   = 2,
    [string]$Cp     = '',
    [switch]$MassErase
)
$ErrorActionPreference = "Stop"
# 找 STM32CubeProgrammer CLI（装上就自动生效）：-Cp > 环境变量 > PATH > C:/D: 常见安装位置
$cp = ''
if ($Cp) { $cp = $Cp }
elseif ($env:STM32_PROGRAMMER_CLI) { $cp = $env:STM32_PROGRAMMER_CLI }
else {
    $g = Get-Command STM32_Programmer_CLI.exe -ErrorAction SilentlyContinue
    if ($g) { $cp = $g.Source }
    if (-not $cp) {
        # 先按已知安装布局点路径（瞬时，2026-09-30 实测：递归扫 D:\APP 会卡住十分钟）
        $pats = @(
            'C:\ST\STM32CubeCLT_*\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',
            'D:\ST\STM32CubeCLT_*\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',
            'D:\APP\ST\STM32CubeCLT_*\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',
            'C:\Program Files\STMicroelectronics\STM32CubeCLT_*\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',
            'D:\Program Files\STMicroelectronics\STM32CubeCLT_*\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',
            'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
        )
        $cp = Get-ChildItem -Path $pats -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $cp) {
        $roots = @('C:\ST','D:\ST','C:\Program Files\STMicroelectronics','C:\Program Files (x86)\STMicroelectronics',
                   'D:\Program Files\STMicroelectronics','D:\STMicroelectronics')
        $cp = Get-ChildItem -Path $roots -Filter 'STM32_Programmer_CLI.exe' -Recurse -Depth 6 -ErrorAction SilentlyContinue |
              Select-Object -First 1 -ExpandProperty FullName
    }
}
$verifier = Join-Path $PSScriptRoot 'verify_readback.ps1'
if (-not (Test-Path $Bin))   { throw "找不到 bin：$Bin" }
if (-not (Test-Path $JLink)) { throw "找不到 JLink.exe：$JLink（可用 -JLink 指定）" }

$tmp = Join-Path $env:TEMP 'haixin_flash'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$readback = Join-Path $tmp 'readback.bin'
$size = (Get-Item $Bin).Length
$len4 = [int]([math]::Ceiling($size / 4.0) * 4)
$max  = [Math]::Max(1, $Retries)
Write-Host "bin = $Bin（$size 字节，device=$Device）"

# 擦除范围：NVRAM（菜单设置/黑白标定）在 Flash 最后一页 = 0x0801F800（App/Svc/nvram.c），
# 链接脚本也只给 app 126K。默认**不显式擦除**：CubeProgrammer 的下载自带"按镜像范围擦扇区"
# （--skipErase 才是跳过它），范围就是 app 的扇区 0..$secEnd，NVRAM 页不在里面，所以烧完设置还在。
# 别用 `-e [0 N]`：这颗 CLI 在这种器件上按自己的扇区编号解释，实测报 "Sector 0032 does not exist"。
# 2026-09-30 复查（用户 m03936）：这颗 G474 实际是 **2 bank × 32 页 × 2KB**（HAL 用的那个
# FLASH_OPTR DBANK 位读出来是 0，不可信 —— 见 memory/PROJECT.md 的「NVRAM 页擦除」坑），
# 但**地址→扇区号仍然是线性的**（扇区 k = 0x08000000 + k*2KB），所以下面的算法照旧有效：
# 0x0801F800 就是扇区 63，97272 字节的 app 只占扇区 0..47，两边都不重叠。
$NvPage  = 0x0801F800
$SecSize = 0x800            # 2KB/扇区；128KB = 64 个扇区（0..63），地址线性编号
$appEnd  = 0x08000000 + $size
if ($appEnd -gt $NvPage) { throw ("bin 末尾 0x{0:X} 已伸进 NVRAM 页 0x0801F800，拒绝烧录" -f $appEnd) }
$secEnd = [int]([math]::Ceiling($size / [double]$SecSize)) - 1
if ($secEnd -lt 0)  { $secEnd = 0 }
if ($secEnd -gt 62) { throw "算出的末扇区 $secEnd 落在 NVRAM 页（63），拒绝烧录" }

if ($cp -and (Test-Path $cp)) {
    $cpArgs = @('-c', 'port=JLINK')
    if ($MassErase) {
        $cpArgs += @('-e', 'all')
        Write-Host "== 前置：CubeProgrammer 整片擦除(-e all) + 编程 + 校验（注意：会清掉 NVRAM 设置/标定）=="
    } else {
        Write-Host "== 前置：CubeProgrammer 编程（自带按镜像擦扇区 0..$secEnd，NVRAM 扇区 63 保留）+ 校验 =="
    }
    $cpArgs += @('-d', $Bin, '0x08000000', '-v')
    & $cp @cpArgs | Select-String 'Download verified successfully|Error|error|NVM size'
    if ($LASTEXITCODE -ne 0) { Write-Host "警告：CubeProgrammer 返回 $LASTEXITCODE，继续走 J-Link，最终仍以 savebin 回读为准" }
} else {
    Write-Host '== 前置：没找到 STM32CubeProgrammer CLI（找过 -Cp/环境变量 STM32_PROGRAMMER_CLI/PATH/C:与D:常见位置）-> 跳过，只走 J-Link =='
}

$jl = Join-Path $tmp 'flash.jlink'
Set-Content -Encoding ASCII $jl @(
    "device $Device"
    'si SWD'
    'speed 4000'
    'connect'
    'h'
    'w4 E000EDFC 00000000'
    'r'
    'w4 E000EDFC 00000000'
    "loadbin $Bin 0x08000000"
    'mem32 0x40022010, 1'
    'r'
    'w4 E000EDFC 00000000'
    "savebin $readback 0x08000000 $($len4.ToString('X'))"
    'go'
    'qc'
)

$ok = $false
for ($try = 1; $try -le $max; $try++) {
    Write-Host "== 第 $try/$max 次：J-Link 写 + savebin 回读 =="
    if (Test-Path $readback) { Remove-Item $readback -Force }
    $jtxt = (& $JLink -CommanderScript $jl -ExitOnError 0 2>&1 | Out-String)
    $jtxt -split "`r?`n" | Select-String -SimpleMatch 'Flash download','Programming failed','Failed to restore','Error while' | ForEach-Object { "   [J-Link] " + $_.Line }
    # 擦/写错误标志是粘住的：非 0 就说明刚才那次编程可能根本没生效（成功信息不算数，见文件头）
    $m = [regex]::Match($jtxt, '40022010\s*=\s*([0-9A-Fa-f]{8})')
    if ($m.Success) {
        $sr = [Convert]::ToUInt32($m.Groups[1].Value, 16)
        if (($sr -band 0xC37A) -ne 0) {
            Write-Host ("警告：FLASH->SR = 0x{0:X8}（PGAERR|PGSERR 之类还粘着）—— 这次擦/写很可能没真正生效，先给板子断电重上电再烧一次" -f $sr)
        }
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $verifier -Bin $Bin -Readback $readback
    if ($LASTEXITCODE -eq 0) { $ok = $true; break }
    Write-Host "第 $try 次回读没过，重试..."
}
if (-not $ok) { throw "烧录失败：$max 次回读都没过" }
Write-Host '烧录完成：回读一致（DEMCR 已清、程序已 go）。'
