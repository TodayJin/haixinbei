"""上位机桥的入口：`python -m bridge.main --sim` / `--serial COM26`。

契约 tools/console/docs/API.md 是冻结的；这个文件只负责选择链路、起 HTTP/SSE 服务、
把 URL 打出来（可选自动开浏览器）。
"""
from __future__ import annotations

import argparse
import os
import sys
import threading
import time
import webbrowser

from .link import available_ports, find_default_port
from .server import Bridge, serve

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE_DIR = os.path.dirname(HERE)


def parse_args(argv=None):
    ap = argparse.ArgumentParser(prog="console-bridge",
                                 description="全地形机器人上位机桥（HTTP + SSE）")
    ap.add_argument("--host", default="127.0.0.1", help="监听地址（默认只给本机）")
    ap.add_argument("--port", type=int, default=8770, help="HTTP 端口，默认 8770")
    ap.add_argument("--sim", action="store_true", help="内置仿真车（无车也能跑，契约要求）")
    ap.add_argument("--serial", metavar="COMX", help="串口设备名，例如 COM26")
    ap.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("--replay", metavar="FILE.jsonl", help="回放录制文件")
    ap.add_argument("--replay-speed", type=float, default=1.0, help="回放倍速，默认 1.0")
    ap.add_argument("--web", metavar="DIR", help="前端 dist 目录（默认 tools/console/web/dist）")
    ap.add_argument("--record", metavar="FILE.jsonl", help="启动即开始录制的文件")
    ap.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    ap.add_argument("--list-ports", action="store_true", help="列出串口后退出")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.list_ports:
        ports = available_ports()
        if not ports:
            print("没找到任何串口（没装 pyserial？）")
            return 1
        for p in ports:
            mark = " ← 像是车上的" if p["likely"] else ""
            print(f"{p['device']:8s} {p['description']}{mark}")
        print(f"\n建议：--serial {find_default_port()}")
        return 0

    mode = "sim" if args.sim else ("replay" if args.replay else "serial")
    b = Bridge(mode=mode, port=args.serial or "", baud=args.baud,
               replay=args.replay or "", replay_speed=args.replay_speed,
               web_root=args.web or "", console_dir=CONSOLE_DIR)
    try:
        b.start()
    except SystemExit as e:
        print(f"启动失败：{e}")
        return 2
    except Exception as e:
        print(f"启动失败：{type(e).__name__}: {e}")
        return 2

    if args.record:
        b.console.record_start(args.record)

    httpd = serve(b, args.host, args.port)
    url = f"http://{args.host}:{args.port}/"
    st = b.runner.status()
    print("=" * 66)
    print("  全地形机器人 · 上位机")
    print(f"  链路    : {st['mode']} {st['port']} @{st['baud']}"
          + ("（内置仿真车）" if st["mode"] == "sim" else ""))
    print(f"  界面    : {url}")
    if not os.path.isfile(os.path.join(b.web_root, "index.html")):
        print(f"  注意    : 前端还没构建（{b.web_root}），先跑：")
        print("            cd tools/console/web && npm install && npm run build")
    print(f"  录制    : {'开' if b.console.recording else '关'}"
          + (f" → {b.console.rec_path}" if b.console.rec_path else ""))
    print("  停止    : Ctrl+C")
    print("=" * 66)

    if not args.no_browser:
        threading.Thread(target=lambda: (time.sleep(0.7), webbrowser.open(url)),
                         daemon=True).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n收到 Ctrl+C，正在收尾……")
    finally:
        try:
            b.console.record_stop()
        except Exception:
            pass
        b.stop()
        httpd.shutdown()
        print("已停止（链路关闭、录制收尾）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
