r"""链路体检：不启动桥、不起网页，直接按一串波特率静听串口，报每个波特率下
「收到多少字节 / 解出多少合法帧 / CRC 错几次」。

为什么需要它：2026-10-03 把 LPUART1 从 115200 提到 **921600** 之后，真车桥在 921600 下
一个字节都没收到（`GET /api/link` = `rx:0`、`fw:null`）。这两种情况必须分开：

- **波特率不匹配**的经典表现是收到**乱码** —— `rx>0` 但一帧都解不出（CRC 错/重同步乱涨）；
- **干净的 0 字节**更像线没通、探针不支持这个波特率、或者固件根本没在发。

这个脚本一眼分开这两种情况，并且能把回退梯子（921600 → 460800 → 230400 → 115200）
一次性扫完，不用反复改桥参数、反复看网页。判定逻辑（`decode_bytes` / `verdict`）
是纯函数，由 `tools/console/tests/test_probe.py` 用合成字节钉住。

安全（不带电）：**默认只收不发**，一个字节都不往线上写。加 `--hello` 才会在监听结束后补发
一帧 HELLO_REQ（只读命令，固件回 T_HELLO）。波特率不匹配时那串字节在车上看来是
乱码，理论上极小概率被解析成别的帧类型 —— 所以默认关；要开请先把车轮架空。

安全（波特率）：**J-Link 的 CDC VCOM 实测上限 230400** —— 试 460800/921600 不但打不开，
还会把探针打进 bootloader 态（`USB\VID_1366&PID_0101`，只能物理拔插 USB 才能恢复）。
所以 J-Link 口上超过 230400 的档会被自动跳过（`bridge/serial_safety.py`），
警告打在 stderr；确实要试请显式 `--force-baud`。

用法（工作目录必须是 `tools/console`）：

    python -m bridge.probe --list
    python -m bridge.probe --port COM26
    python -m bridge.probe --port COM26 --bauds 921600,460800 --seconds 3
"""
from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass, field

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - 环境问题，不是逻辑问题
    serial = None  # type: ignore[assignment]
    list_ports = None  # type: ignore[assignment]

from .console_io import harden_stdio
from .protocol import TYPE_NAMES, Frame, FrameDecoder, decode_hello, frame_hello_req
from .serial_safety import cap_bauds

# 回退梯子：从用户要的 921600 一路降到历史值
DEFAULT_BAUDS = (921600, 460800, 230400, 115200)


def decode_bytes(data: bytes) -> tuple[list[Frame], int, int]:
    """把一段字节喂给协议解码器，返回 (合法帧, CRC 错次数, 重同步次数)。

    单独抽出来是为了能在 pytest 里拿**合成字节**钉住判定逻辑，不用真串口、不用真车。
    """
    dec = FrameDecoder()
    frames = dec.feed(data)
    return frames, dec.crc_errors, dec.resyncs


def verdict(rx_bytes: int, frames: int, crc_errors: int) -> str:
    """把三个数翻译成人话 —— 这行字就是现场判断的依据。"""
    if rx_bytes == 0:
        return "0 字节：这条线上什么都没来（板子没发 / 线没通 / 探针这个波特率不通）"
    if frames == 0:
        return "有字节但一帧都解不出：典型的波特率不匹配（收到的是乱码）"
    if crc_errors:
        return "%d 帧，但夹着 %d 次 CRC 错：波特率或线材在边缘" % (frames, crc_errors)
    return "%d 帧，零 CRC 错" % frames


@dataclass
class Result:
    baud: int
    rx_bytes: int = 0
    frames: list[Frame] = field(default_factory=list)
    crc_errors: int = 0
    resyncs: int = 0
    hello: dict | None = None
    error: str = ""

    @property
    def sentence(self) -> str:
        if self.error:
            return "打不开：%s" % self.error
        return verdict(self.rx_bytes, len(self.frames), self.crc_errors)

    @property
    def usable(self) -> bool:
        """能不能拿来当链路：解出过帧、且一帧 CRC 错都没有。"""
        return bool(self.frames) and self.crc_errors == 0 and not self.error

    def type_histogram(self) -> str:
        counts: dict[int, int] = {}
        for f in self.frames:
            counts[f.type] = counts.get(f.type, 0) + 1
        parts = [
            "%s×%d" % (TYPE_NAMES.get(t, "0x%02X" % t), n)
            for t, n in sorted(counts.items(), key=lambda kv: -kv[1])
        ]
        return "、".join(parts) if parts else "—"


def probe(port: str, baud: int, seconds: float, hello: bool = False) -> Result:
    """静听 `port`（8N1）`seconds` 秒，把收到的字节按协议解一遍。"""
    r = Result(baud=baud)
    if serial is None:  # pragma: no cover
        r.error = "没装 pyserial（C:\\Python314\\python.exe -m pip install pyserial）"
        return r
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except Exception as exc:  # 口被占/不存在/驱动问题
        r.error = str(exc)
        return r
    dec = FrameDecoder()
    try:
        ser.reset_input_buffer()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                r.rx_bytes += len(chunk)
                r.frames.extend(dec.feed(chunk))
        if hello:
            ser.write(frame_hello_req(1))
            ser.flush()
            deadline = time.monotonic() + 0.6
            while time.monotonic() < deadline:
                chunk = ser.read(4096)
                if chunk:
                    r.rx_bytes += len(chunk)
                    r.frames.extend(dec.feed(chunk))
    finally:
        try:
            ser.close()
        except Exception:  # pragma: no cover
            pass
    r.crc_errors = dec.crc_errors
    r.resyncs = dec.resyncs
    for f in r.frames:
        if f.type == 0x40:  # T_HELLO：固件横幅（名字/版本/能力位）
            r.hello = decode_hello(f.payload)
            break
    return r


def _report(r: Result) -> None:
    if r.error:
        print("  打不开：%s" % r.error)
        return
    print("  字节 %d / 合法帧 %d / CRC 错 %d / 重同步 %d"
          % (r.rx_bytes, len(r.frames), r.crc_errors, r.resyncs))
    print("  判定：%s" % r.sentence)
    if r.frames:
        print("  帧类型：%s" % r.type_histogram())
        f = r.frames[0]
        print("  第一帧：%s seq=%d len=%d" % (f.type_name, f.seq, len(f.payload)))
    if r.hello:
        print("  固件横幅：%s" % r.hello)


def main(argv: list[str] | None = None) -> int:
    harden_stdio()          # 结论行里的 ⇒ 在 GBK 控制台上编不出来，会崩在最后一行
    ap = argparse.ArgumentParser(
        prog="python -m bridge.probe",
        description="链路体检：按一串波特率静听串口，报收了多少字节、解出多少合法帧",
    )
    ap.add_argument("--port", help="串口名，例如 COM26")
    ap.add_argument("--bauds", default=",".join(str(b) for b in DEFAULT_BAUDS),
                    help="逗号分隔的波特率，默认 %s" % ",".join(str(b) for b in DEFAULT_BAUDS))
    ap.add_argument("--seconds", type=float, default=2.0, help="每个波特率听多久（默认 2 秒）")
    ap.add_argument("--hello", action="store_true",
                    help="监听后再补发一帧 HELLO_REQ（默认只收不发；开之前把车轮架空）")
    ap.add_argument("--list", action="store_true", help="只列出本机串口")
    ap.add_argument("--force-baud", action="store_true", dest="force_baud",
                    help="不跳过 J-Link CDC 口上 >230400 的档（会让探针崩进 bootloader 态，慎用）")
    args = ap.parse_args(argv)

    if list_ports is not None and (args.list or not args.port):
        ports = list(list_ports.comports())
        if ports:
            for p in ports:
                print("%-8s %s" % (p.device, p.description))
        else:
            print("（本机一个串口都没有）")
        if not args.port:
            print("没给 --port：上面这些口里挑一个。注意口被桥占着时打不开。", file=sys.stderr)
            return 0 if args.list else 2

    bauds: list[int] = []
    for tok in str(args.bauds).split(","):
        tok = tok.strip()
        if tok:
            bauds.append(int(tok))

    bauds, warn = cap_bauds(args.port, bauds, args.force_baud)
    if warn:
        print("警告：%s" % warn, file=sys.stderr)

    print("端口 %s，每档听 %.1fs，%s" % (args.port, args.seconds,
                                       "监听后补发 HELLO_REQ" if args.hello else "只收不发"))
    results = [probe(args.port, b, args.seconds, args.hello) for b in bauds]
    for r in results:
        print("")
        print("=== @ %d ===" % r.baud)
        _report(r)

    usable = [r for r in results if r.usable]
    opened = [r for r in results if not r.error]
    silent = [r for r in opened if r.rx_bytes == 0]
    print("")
    if usable:
        best = max(usable, key=lambda r: r.baud)
        print("结论：最高可用波特率 = %d（把桥的 --baud 与固件的 DL_BAUD 都用它）" % best.baud)
        return 0
    if not opened:
        print("结论：所有档都**打不开端口** ⇒ 与波特率无关，先解决口本身："
              "（a）口被桥或别的程序占着（`--list` 看还在不在）、（b）探针的 CDC 串口没枚举出来"
              "（设备管理器里有没有 JLink CDC UART Port）、（c）线/探针被拔了或口号变了")
        return 2
    if len(silent) == len(opened):
        print("结论：能打开但%s都是 0 字节 ⇒ 不是波特率的事："
              "先查（a）板子 LPUART1 的 TX/RX 有没有接到探针（TX/RX 是不是交叉了）、"
              "（b）固件是不是在发（RTT 看 1Hz 的 HOST 行 tx 有没有涨）"
              % ("所有波特率" if len(opened) == len(results) else "能打开的那几档"))
        return 1
    print("结论：有字节但解不出帧 ⇒ 波特率不匹配或线材太差；"
          "按上面哪一档『有字节』去比对板子实际用的波特率")
    return 1


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
