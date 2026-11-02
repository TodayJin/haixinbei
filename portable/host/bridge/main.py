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
            # ★2026-10-14：把 rank 也打出来（0 = 车上的 DAPLink / 1 = J-Link / 2 = 其它 USB /
            #   3 = 未知 / 4 = 蓝牙）。只亮"像是车上的"会让人以为其它口也能用。
            mark = " ← 像是车上的" if p["likely"] else f"   (rank {p.get('rank', '?')})"
            print(f"{p['device']:8s} {p['description']}{mark}")
        pick = find_default_port()
        if pick:
            print(f"\n建议：--serial {pick}")
        else:
            # ★2026-10-14：以前这里会把上面任意一个口（比如 ESP32）当"建议"报出来，
            #   用户照着连上 ⇒ 口能开、却不回话。现在**明确说找不到车**。
            print("\n⚠ 上面这些口都**不是车上的**（车上的 DAPLink 是 VID_FAED:4870 / CMSIS-DAP，"
                  "或 J-Link）。")
            print("  请确认：① 车先上电 ② 再插 DAPLink 的 USB（这台机器 SWD 和 CDC 同一颗芯片，"
                  "上位机连过之后经常要重插）③ 稍等 5 秒再试。")
            print("  如果车确实接在别的口上，用 `-Port COMx` 明确指定。")
        return 0

    mode = "sim" if args.sim else "serial"
    port = args.serial or ""
    if mode == "serial":
        # ★2026-10-14：两处改动
        #   ① 找不到车的口就**明确失败**（以前会传空串下去、由下层再"兜底"挑一个别的口，
        #      于是校验的是一个口、真正打开的是另一个口）；
        #   ② `port` 解析一次就**复用它**（原来是 `args.serial or find_default_port()` 校验，
        #      却又 `args.serial or ""` 传下去 ⇒ 下面会重新自动挑一次，两次可能不同）。
        port = port or find_default_port()
        if not port:
            print("启动失败：没找到**车上的**串口（DAPLink VID_FAED:4870 / CMSIS-DAP，或 J-Link）。")
            print("  · 车先上电，再插 DAPLink 的 USB；这台机器 SWD 与 CDC 是同一颗芯片，")
            print("    上位机用过之后经常要重插才能再被认出来。")
            print("  · `-ListPorts` 看有哪些口；用 `-Port COMx` 明确指定。")
            return 2
        err = check_baud(port, args.baud, args.force_baud)
        if err:
            print(f"启动失败：{err}")
            return 2
    hi_res = False if args.low_res_timer else raise_timer_resolution()
    b = Bridge(mode=mode, port=port, baud=args.baud,
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
        # ★2026-10-11：这里**故意保持最简单的 `webbrowser.open`**。
        #   之前试过两条"绕过 Edge 权限框"的路，都白搭、而且会把问题弄反：
        #     · 判断管理员就干脆不开 —— 用户要的就是自动开，不行；
        #     · 改走 `explorer.exe <url>` 转发 —— Explorer 自己也是**普通权限**的，
        #       而那个框恰恰是"普通权限进程去叫提权的 Edge"，等于火上浇油。
        #   真正的根因是**Edge 那个实例的权限和调用方不一致**：
        #   弹窗上点一次「是」（= 把 Edge 重启成普通权限）就一劳永逸；
        #   或者把 Edge 完全关掉、让上位机自己打开它，两者权限自然一致。
        #   这跟上位机无关，代码里也管不了，所以不在这里做任何"聪明事"。
        threading.Thread(target=lambda: (time.sleep(0.7), webbrowser.open(url)),
                         daemon=True).start()
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
