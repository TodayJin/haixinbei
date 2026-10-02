"""上位机入口（调参台）。

用法：
    python main.py                # 演示数据模式（无硬件也能看波形、试调参）
    python main.py --port COM5    # 连真实串口（无线下载器的虚拟串口）
    python main.py --csv rec.csv  # 回放一段录制
    python main.py --ui tk        # 用 tkinter 版界面（零依赖备胎）
    python main.py --offline      # 只开界面，不自动连接
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description="全地形机器人上位机（调参台）")
    ap.add_argument("--port", help="串口号，如 COM5")
    ap.add_argument("--baud", type=int, default=115200, help="波特率（默认 115200）")
    ap.add_argument("--csv", help="回放指定的 CSV 文件")
    ap.add_argument("--sim", action="store_true", help="进演示数据模式（默认就是）")
    ap.add_argument("--speed", type=float, default=1.0, help="演示数据/回放倍速")
    ap.add_argument("--offline", action="store_true", help="只开界面，不自动连接")
    ap.add_argument("--ui", choices=("qt", "tk"), default="qt",
                    help="界面库：qt（PySide6，默认）/ tk（tkinter，零依赖备胎）")
    return ap.parse_args(argv)


def _ui_available() -> bool:
    try:
        import PySide6  # noqa: F401
        return True
    except Exception:                 # 不只是 ImportError：Qt 的 DLL 加载失败也要能兜住
        return False


def _speed_text(speed: float) -> str:
    return str(int(speed)) if float(speed).is_integer() else str(speed)


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.ui == "tk" or not _ui_available():
        if args.ui == "qt":
            print("未安装 PySide6（或加载失败），退回 tkinter 界面。"
                  "建议：pip install PySide6 pyqtgraph")
        return _main_tk(args)
    return _main_qt(args)


def _main_qt(args) -> int:
    from PySide6.QtWidgets import QApplication

    from ui_qt import theme as T
    from ui_qt.window import MainWindow

    qapp = QApplication.instance() or QApplication([])
    qapp.setStyleSheet(T.QSS)
    win = MainWindow()
    win.speed.setCurrentText(_speed_text(args.speed))

    if args.csv:
        win.select_mode("回放")
        try:
            win.dev.open_replay(args.csv, speed=args.speed)
            win.log_line("回放：%s" % args.csv)
        except Exception as exc:
            win.log_line("回放失败：%s（改为演示数据）" % exc)
            win.select_mode("演示数据")
            win.toggle_connect()
    elif args.port:
        win.select_mode("串口")
        win.baud.setCurrentText(str(args.baud))
        win.toggle_connect()
    elif not args.offline:
        win.select_mode("演示数据")
        win.toggle_connect()

    win.show()
    return qapp.exec()


def _main_tk(args) -> int:
    from app import HostApp

    app = HostApp()
    app.speed_var.set(_speed_text(args.speed))

    if args.csv:
        app.mode.set("回放")
        try:
            app.dev.open_replay(args.csv, speed=args.speed)
            app._log("回放：%s" % args.csv)
        except Exception as exc:
            app._log("回放失败：%s（改为演示数据）" % exc)
            app.mode.set("演示数据")
            app.toggle_connect()
    elif args.port:
        app.mode.set("串口")
        app.baud_var.set(str(args.baud))
        for v in app.port_box.cget("values"):
            if str(v).startswith(args.port):
                app.port_var.set(v)
                break
        else:
            app.port_var.set(args.port)
        app.toggle_connect()
    elif not args.offline:
        app.mode.set("演示数据")
        app.toggle_connect()

    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
