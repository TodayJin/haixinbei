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
import subprocess
import sys
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
    ap.add_argument("--baud", type=int, default=921600,
                    help="波特率，默认 921600（和板上固件的 DL_BAUD 一致；车上第二颗探针的 VCOM 实测能到 3 Mbaud）")
    ap.add_argument("--web", metavar="DIR", help="前端 dist 目录（默认 tools/console/web/dist）")
    ap.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    ap.add_argument("--list-ports", action="store_true", help="列出串口后退出")
    ap.add_argument("--low-res-timer", action="store_true",
                    help="不提高 Windows 计时器精度（默认提高：链路轮询才跑得动 100Hz 遥测）")
    ap.add_argument("--sse-batch-ms", type=int, default=SSE_BATCH_MS_DEFAULT, metavar="N",
                    help=f"SSE 按 N 毫秒攒批，连续遥测合成一条 telem_batch（默认 "
                         f"{SSE_BATCH_MS_DEFAULT}；0 = 不合并，一事件一报文）")
    ap.add_argument("--force-baud", action="store_true", dest="force_baud",
                    help="不拦超过该探针实测上限的波特率（旧探针 PID_0105 上限 230400，"
                         "超了会崩进 bootloader 态、只能物理拔插恢复；慎用）")
    return ap.parse_args(argv)


def _is_admin() -> bool:
    """当前进程是不是以管理员权限跑着（Windows）。拿不到就当 False。"""
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


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

    mode = "sim" if args.sim else "serial"
    if mode == "serial":
        err = check_baud(args.serial or find_default_port(), args.baud, args.force_baud)
        if err:
            print(f"启动失败：{err}")
            return 2
    hi_res = False if args.low_res_timer else raise_timer_resolution()
    b = Bridge(mode=mode, port=args.serial or "", baud=args.baud,
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
    print("  计时器  : " + ("1ms（已提高：链路轮询 ~180 次/s，100Hz 遥测不再成串到）"
                            if hi_res else "系统默认（Windows 上约 15.6ms）"))
    print("  SSE 合并: " + (f"{b.sse_batch_ms}ms（连续遥测合成一条 telem_batch）"
                            if b.sse_batch_ms else "关（一事件一报文）"))
    print("  停止    : Ctrl+C")
    print("=" * 66)

    if not args.no_browser:
        # ★2026-10-11：**管理员权限下不要直接 webbrowser.open()**。
        #   提升权限的进程去开 Edge/Chrome 时，Windows 会弹
        #   「Microsoft Edge 未响应，因为现有实例正在以提升的权限运行。是否要用普通权限重启现有实例？」
        #   —— 用户实测每次启动都弹（他这台机器所有进程都是管理员，UAC 关着/内置 Administrator）。
        #   绕过办法：**走 explorer.exe 转发** —— 已经在跑的 Explorer 是**普通权限**的，
        #   由它去开浏览器就不会有跨权限级别的问题。explorer 转发失败再退回 webbrowser。
        def _open():
            time.sleep(0.7)
            try:
                if sys.platform == "win32" and _is_admin():
                    # ★注意：explorer.exe **即使成功也返回 1**（已知怪癖）——
                    #   千万别写成 `if Popen(...).wait() == 0: return`，那样永远等不到 0、
                    #   每次都会退回 webbrowser.open()，弹窗照旧。发出去就算成功。
                    subprocess.Popen(["explorer.exe", url],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    return
            except Exception:
                pass
            try:
                webbrowser.open(url)
            except Exception:
                pass

        threading.Thread(target=_open, daemon=True).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n收到 Ctrl+C，正在收尾……")
    finally:
        b.stop()
        httpd.shutdown()
        if hi_res:
            drop_timer_resolution()
        print("已停止（链路关闭）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
