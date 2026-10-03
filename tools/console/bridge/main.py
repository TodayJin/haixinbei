"""上位机桥的入口：`python -m bridge.main --sim` / `--serial COM26`。

契约 tools/console/docs/API.md 是冻结的；这个文件只负责选择链路、起 HTTP/SSE 服务、
把 URL 打出来（可选自动开浏览器）。
"""
from __future__ import annotations

import argparse
import ctypes
import os
import sys
import threading
import time
import webbrowser

from .console_io import harden_stdio
from .link import available_ports, find_default_port
from .serial_safety import check_baud
from .server import Bridge, serve, SSE_BATCH_MS_DEFAULT

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE_DIR = os.path.dirname(HERE)


def raise_timer_resolution() -> bool:
    """Windows 默认计时器精度约 15.6ms ⇒ `LinkRunner` 里那个 5ms 的 `Event.wait`
    实际只醒 64 次/s，车上 100Hz 的遥测只能每 15.6ms 成串取一次（实测：间隔 p50
    14.8ms、指令往返 p50 16ms）。`timeBeginPeriod(1)` 后实测链路轮询 64 → 179 次/s。

    非 Windows 直接回 False（POSIX 的定时器本来就够细）。
    """
    if os.name != "nt":
        return False
    try:
        return ctypes.windll.winmm.timeBeginPeriod(1) == 0      # 0 = TIMERR_NOERROR
    except Exception:
        return False


def drop_timer_resolution() -> None:
    if os.name != "nt":
        return
    try:
        ctypes.windll.winmm.timeEndPeriod(1)
    except Exception:
        pass


def parse_args(argv=None):
    ap = argparse.ArgumentParser(prog="console-bridge",
                                 description="全地形机器人上位机桥（HTTP + SSE）")
    ap.add_argument("--host", default="127.0.0.1", help="监听地址（默认只给本机）")
    ap.add_argument("--port", type=int, default=8770, help="HTTP 端口，默认 8770")
    ap.add_argument("--sim", action="store_true", help="内置仿真车（无车也能跑，契约要求）")
    ap.add_argument("--serial", metavar="COMX", help="串口设备名，例如 COM26")
    ap.add_argument("--baud", type=int, default=921600, help="波特率，默认 921600（和固件一致）")
    ap.add_argument("--replay", metavar="FILE.jsonl", help="回放录制文件")
    ap.add_argument("--replay-speed", type=float, default=1.0, help="回放倍速，默认 1.0")
    ap.add_argument("--web", metavar="DIR", help="前端 dist 目录（默认 tools/console/web/dist）")
    ap.add_argument("--record", metavar="FILE.jsonl", help="启动即开始录制的文件")
    ap.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    ap.add_argument("--list-ports", action="store_true", help="列出串口后退出")
    ap.add_argument("--low-res-timer", action="store_true",
                    help="不提高 Windows 计时器精度（默认提高：链路轮询才跑得动 100Hz 遥测）")
    ap.add_argument("--sse-batch-ms", type=int, default=SSE_BATCH_MS_DEFAULT, metavar="N",
                    help=f"SSE 按 N 毫秒攒批，连续遥测合成一条 telem_batch（默认 "
                         f"{SSE_BATCH_MS_DEFAULT}；0 = 不合并，一事件一报文）")
    ap.add_argument("--force-baud", action="store_true", dest="force_baud",
                    help="不拦 J-Link CDC 口上 >230400 的波特率（会让探针崩进 bootloader 态，慎用）")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    harden_stdio()          # 横幅/判定里的 ⇒ 在 GBK 控制台上编不出来
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
    if mode == "serial":
        err = check_baud(args.serial or find_default_port(), args.baud, args.force_baud)
        if err:
            print(f"启动失败：{err}")
            return 2
    hi_res = False if args.low_res_timer else raise_timer_resolution()
    b = Bridge(mode=mode, port=args.serial or "", baud=args.baud,
               replay=args.replay or "", replay_speed=args.replay_speed,
               web_root=args.web or "", console_dir=CONSOLE_DIR,
               sse_batch_ms=args.sse_batch_ms)
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
    print("  计时器  : " + ("1ms（已提高：链路轮询 ~180 次/s，100Hz 遥测不再成串到）"
                            if hi_res else "系统默认（Windows 上约 15.6ms）"))
    print("  SSE 合并: " + (f"{b.sse_batch_ms}ms（连续遥测合成一条 telem_batch）"
                            if b.sse_batch_ms else "关（一事件一报文）"))
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
        if hi_res:
            drop_timer_resolution()
        print("已停止（链路关闭、录制收尾）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
