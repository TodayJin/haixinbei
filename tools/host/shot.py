"""把界面渲染成 PNG —— 改界面时的回归对比手段，也可当文档配图。

用法：
    python shot.py                      # 真实平台，赛道页，跑 9 秒（窗口会闪一下）
    python shot.py --offscreen          # 离屏（无字体，只看布局）
    python shot.py --tab 1 --seconds 5  # 波形页
    python shot.py --all                # 六个页各存一张
"""

from __future__ import annotations

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.chdir(HERE)

TABS = ["map", "wave", "param", "cmd", "record", "log"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="界面截图")
    ap.add_argument("--offscreen", action="store_true", help="离屏渲染（无字体）")
    ap.add_argument("--tab", type=int, default=0, help="标签页序号 0..5")
    ap.add_argument("--all", action="store_true", help="六个页各存一张")
    ap.add_argument("--seconds", type=float, default=9.0, help="先跑多久积累数据")
    ap.add_argument("--start", action="store_true", default=True,
                    help="自动启动任务 A（默认开）")
    ap.add_argument("--outdir", default=os.path.join(
        os.path.dirname(os.path.dirname(HERE)), "out", "host"))
    args = ap.parse_args(argv)

    if args.offscreen:
        os.environ["QT_QPA_PLATFORM"] = "offscreen"

    from PySide6.QtWidgets import QApplication
    import protocol as P
    from ui_qt import theme as T
    from ui_qt.window import MainWindow

    qapp = QApplication.instance() or QApplication([])
    qapp.setStyleSheet(T.QSS)
    win = MainWindow()
    win.resize(1400, 880)
    win.show()
    win.mode.setCurrentText("模拟器")
    win.speed.setCurrentText("20")
    win.toggle_connect()
    if args.start:
        win.dev.send_cmd(P.CMD_START_A)

    t0 = time.time()
    while time.time() - t0 < args.seconds:
        qapp.processEvents()
        time.sleep(0.008)

    os.makedirs(args.outdir, exist_ok=True)
    todo = range(6) if args.all else [args.tab]
    for tab in todo:
        win.tabs.setCurrentIndex(tab)
        for _ in range(8):
            qapp.processEvents()
            time.sleep(0.03)
        path = os.path.join(args.outdir, "ui_%s.png" % TABS[tab])
        win.grab().save(path)
        print("saved", path)
    if win._last_pose:
        print("得分", win._last_pose.score, "完成", win._last_pose.done_ids)
    win.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
