# 重新生成「任务程序」金样本：C(taskgen.c + 真 task_prog.c) → fixtures/task.golden.json → web/src/lib/task.golden.ts
#
#   pwsh -File tools/console/tests/c/make_golden.ps1
#
# 改任务格式之后必须跑这一条，然后跑 test_task_bytes.py 与 web 的 npm test，
# 三处（固件 / 桥 / 上位机）才算是同一份格式。
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$py = if ($env:PYTHON) { $env:PYTHON } elseif (Test-Path 'C:\Python314\python.exe') { 'C:\Python314\python.exe' } else { 'python' }

# 中文 Windows 的控制台是 GBK：让 python 明确按 UTF-8 吐字，免得名字变成乱码
$env:PYTHONIOENCODING = 'utf-8'
& $py (Join-Path $here 'make_golden.py')
if ($LASTEXITCODE -ne 0) { throw "make_golden.py 失败（exit=$LASTEXITCODE）" }
Write-Host "金样本已更新；仓库根 = $repo"
