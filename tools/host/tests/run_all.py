"""在独立进程里依次跑三个测试文件并汇总。

为什么不用 unittest discover 一把跑完：
tkinter 在同一个进程里反复创建/销毁 Tk 解释器会触发
Tcl_AsyncDelete: async handler deleted by the wrong thread，进程直接崩。
所以界面测试必须单独一个进程 —— 这个脚本负责隔离与汇总。

用法：
    cd tools/host && python tests\run_all.py
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.dirname(HERE)
# 界面测试各自独立进程：Qt 与 Tk 不能在同一个进程里共存
FILES = ("test_protocol.py", "test_metrics.py", "test_link.py", "test_launchers.py",
         "test_ui_smoke.py", "test_ui_qt.py")


def run_one(name: str) -> tuple[int, bool, float, str]:
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    t0 = time.monotonic()
    proc = subprocess.run(
        [sys.executable, os.path.join(HERE, name)],
        cwd=PKG, env=env, capture_output=True, text=True,
        encoding="utf-8", errors="replace")
    dt = time.monotonic() - t0
    out = (proc.stdout or "") + (proc.stderr or "")
    m = re.search(r"Ran (\d+) tests?", out)
    n = int(m.group(1)) if m else 0
    ok = proc.returncode == 0 and re.search(r"^OK", out, re.M) is not None
    return n, ok, dt, out


def main() -> int:
    rows = []
    total = 0
    failed = 0
    for name in FILES:
        n, ok, dt, out = run_one(name)
        rows.append((name, n, ok, dt))
        total += n
        if not ok:
            failed += 1
            print("=" * 68)
            print("FAILED: %s" % name)
            print("=" * 68)
            print("\n".join(out.strip().splitlines()[-25:]))
            print()
    print("-" * 68)
    print("%-22s %6s %8s  %s" % ("测试文件", "用例", "耗时", "结果"))
    print("-" * 68)
    for name, n, ok, dt in rows:
        print("%-22s %6d %7.1fs  %s" % (name, n, dt, "通过" if ok else "失败"))
    print("-" * 68)
    print("合计 %d 个用例，%d 个文件失败" % (total, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
