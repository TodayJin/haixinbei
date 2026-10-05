"""两种链路：真车串口 / 内置仿真（契约 tools/console/docs/API.md §1）。

两者对上层是同一个接口：
    link.start()                 打开
    link.poll(now_ms) -> [Frame] 非阻塞收帧（内部自己解码）
    link.send(frame_bytes)       发一帧
    link.status() -> dict        {mode, open, port, baud, rx_bytes, tx_bytes, crc_errors,
                                  resyncs, last_rx_ms, note}
`SerialLink` 在串口被拔掉/报错时不会抛异常给上层：它会关口、记 `note`，
每隔 `RECONNECT_MS` 自己重试，接回来的瞬间 `status().open` 变 True。
"""
from __future__ import annotations

import glob
import threading
import time

from . import protocol as pr

try:
    import serial  # pyserial
    from serial.tools import list_ports
except Exception:                                        # pragma: no cover
    serial = None
    list_ports = None

RECONNECT_MS = 1000
READ_CHUNK = 4096
# 驱动接收缓冲。pyserial 的 Windows 后端固定 `SetupComm(handle, 4096, 4096)`
# （见 serial.serialwin32），4096 B 在 921600/200Hz 遥测下只够 **205ms** ——
# 桥的读线程一旦被别的活（HTTP 线程持 GIL、SSE 扇出、大 JSON）挡住更久，
# 驱动就静默丢字节：表现是 `resyncs`/`crc_errors` 涨，偶尔一条请求的 ACK 恰好
# 被丢掉 ⇒ 上位机报"无应答"（而车其实已经执行了）。这里把它抬到 256KB（≈13s）。
RX_BUFFER = 262144
# 单次 poll 最多搬这么多字节：正常一拍只有 ~100 B，只有积压时才用得上
READ_BUDGET = 262144


def available_ports() -> list[dict]:
    """给 /api/ports 用：机器上能被 pyserial 看见的串口 + 是否像 J-Link/无线下载器。"""
    out = []
    if list_ports is None:
        return out
    for p in list_ports.comports():
        desc = (p.description or "")
        out.append({
            "device": p.device,
            "description": desc,
            "hwid": p.hwid or "",
            "likely": ("JLink" in desc.replace(" ", "")) or ("J-Link" in desc)
                      or ("CDC" in desc) or ("USB" in (p.hwid or "").upper()),
        })
    out.sort(key=lambda d: (not d["likely"], d["device"]))
    return out


class LinkError(RuntimeError):
    pass


class LinkBase:
    mode = "?"
    port = ""
    # 固件 2026-10-05 实机烧的是 DL_BAUD 921600（车上第二颗探针 PID_1024 的 VCOM 能吃），
    # 两边必须一致；改这里要连 main.py --baud / 网页默认值 / board.c 的 DL_BAUD 一起改
    baud = 921600

    def __init__(self):
        self.dec = pr.FrameDecoder()
        self.open_ = False
        self.note = ""
        self.last_rx_ms = 0
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.rx_frames = 0
        self.tx_frames = 0
        # 诊断：驱动缓冲里最多攒过多少字节、两次 poll 之间最长隔了多久。
        # 这两个数是"上位机读得够不够快"的直接证据（见 RX_BUFFER 的注释）。
        self.max_in_waiting = 0
        self.max_gap_ms = 0
        self.last_poll_ms = 0

    # ---- 生命周期
    def start(self) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        self.open_ = False

    # ---- 收发
    def poll(self, now_ms: int) -> list[pr.Frame]:
        raise NotImplementedError

    def send(self, data: bytes) -> int:
        raise NotImplementedError

    def status(self) -> dict:
        return {
            "mode": self.mode, "open": self.open_, "port": self.port, "baud": self.baud,
            "rx_bytes": self.rx_bytes, "tx_bytes": self.tx_bytes,
            "rx_frames": self.rx_frames, "tx_frames": self.tx_frames,
            "crc_errors": self.dec.crc_errors, "resyncs": self.dec.resyncs,
            "max_in_waiting": self.max_in_waiting, "max_gap_ms": self.max_gap_ms,
            "last_rx_ms": self.last_rx_ms, "note": self.note,
        }

    def _frames(self, data: bytes, now_ms: int) -> list[pr.Frame]:
        self.rx_bytes += len(data)
        fs = self.dec.feed(data)
        if fs:
            self.rx_frames += len(fs)
            self.last_rx_ms = now_ms
        return fs


class SerialLink(LinkBase):
    """真车：USB 虚拟串口（无线下载器）/ J-Link VCOM，默认 921600 8N1。"""

    mode = "serial"

    def __init__(self, port: str, baud: int = 921600):
        super().__init__()
        self.port = port
        self.baud = baud
        self.ser = None
        self.next_try_ms = 0
        self._lock = threading.Lock()

    def start(self) -> None:
        if serial is None:
            raise LinkError("没装 pyserial：pip install pyserial")
        self._try_open(time.time() * 1000.0)

    def _try_open(self, now_ms: float) -> None:
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0, write_timeout=0.2)
            # 抬高驱动接收缓冲（Windows 默认 4096 B；posix 后端会忽略这个建议值）。
            # 失败不算错：老驱动/非 Windows 平台照旧用默认值，只是抗卡顿余量小。
            try:
                self.ser.set_buffer_size(rx_size=RX_BUFFER)
            except Exception:
                pass
            self.open_ = True
            self.note = ""
        except Exception as e:
            self.ser = None
            self.open_ = False
            self.note = f"打不开 {self.port}：{e}"
            self.next_try_ms = now_ms + RECONNECT_MS

    def stop(self) -> None:
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        super().stop()

    def poll(self, now_ms: int) -> list[pr.Frame]:
        if self.ser is None:
            if now_ms >= self.next_try_ms:
                self._try_open(now_ms)
            return []
        # 进 poll 的间隔 = 读线程被别的活挡住的最长时间（含发帧、派帧、睡眠）
        if self.last_poll_ms and now_ms - self.last_poll_ms > self.max_gap_ms:
            self.max_gap_ms = int(now_ms - self.last_poll_ms)
        self.last_poll_ms = now_ms
        out: list[pr.Frame] = []
        try:
            # 一拍尽量搬空，而不是"只读一块"：积压时下一拍才能追上
            budget = READ_BUDGET
            while budget > 0:
                n = self.ser.in_waiting
                if n > self.max_in_waiting:
                    self.max_in_waiting = int(n)
                if not n:
                    break
                data = self.ser.read(min(READ_CHUNK, n, budget))
                if not data:
                    break
                out.extend(self._frames(data, int(now_ms)))
                budget -= len(data)
        except Exception as e:
            self.note = f"串口读失败（已断开，自动重连中）：{e}"
            self.stop()
            self.next_try_ms = now_ms + RECONNECT_MS
            return out
        return out

    def send(self, data: bytes) -> int:
        if self.ser is None:
            return 0
        with self._lock:
            try:
                n = self.ser.write(data)
                self.ser.flush()
            except Exception as e:
                self.note = f"串口写失败（已断开，自动重连中）：{e}"
                self.stop()
                self.next_try_ms = time.time() * 1000.0 + RECONNECT_MS
                return 0
        self.tx_bytes += n or 0
        self.tx_frames += 1
        return n or 0


class SimLink(LinkBase):
    """`--sim`：内置仿真车（同一个协议栈），无车也能把整条链路跑通。"""

    mode = "sim"

    def __init__(self, seed: int = 7):
        super().__init__()
        from .sim import SimCar
        self.car = SimCar(seed=seed)
        self.port = "sim"
        self.t0 = time.time() * 1000.0

    def start(self) -> None:
        self.open_ = True
        self.t0 = time.time() * 1000.0
        self.car.start(0)

    def poll(self, now_ms: int) -> list[pr.Frame]:
        ms = int(now_ms - self.t0)
        frames = self.car.poll(ms)
        # 过一遍编解码，保证发出去的和真车一样有帧头/CRC
        raw = b"".join(pr.build_frame(f.type, f.seq, f.payload) for f in frames)
        return self._frames(raw, int(now_ms))

    def send(self, data: bytes) -> int:
        self.tx_bytes += len(data)
        self.tx_frames += 1
        self.car.send(data)
        return len(data)


class LinkRunner:
    """后台线程：每 `period_ms` 调一次 link.poll()，把帧交给 on_frame。"""

    def __init__(self, link: LinkBase, on_frame, period_ms: int = 5):
        self.link = link
        self.on_frame = on_frame
        self.period_ms = period_ms
        self._stop = threading.Event()
        self._th: threading.Thread | None = None
        self.frames_seen = 0

    def start(self):
        self.link.start()
        self._th = threading.Thread(target=self._loop, name="link", daemon=True)
        self._th.start()
        return self

    def _loop(self):
        while not self._stop.is_set():
            now_ms = time.time() * 1000.0
            try:
                for f in self.link.poll(now_ms):
                    self.frames_seen += 1
                    self.on_frame(f, int(now_ms))
            except Exception as e:                   # 链路层异常不许打死线程
                self.link.note = f"链路异常：{e}"
            self._stop.wait(self.period_ms / 1000.0)

    def send(self, data: bytes) -> int:
        return self.link.send(data)

    def stop(self):
        self._stop.set()
        if self._th is not None:
            self._th.join(timeout=1.0)
        self.link.stop()

    def status(self) -> dict:
        s = self.link.status()
        s["frames_seen"] = self.frames_seen
        return s


def find_default_port() -> str:
    """挑一个最像车上的串口：优先名字里带 JLink/CDC 的，其次第一个非蓝牙口。"""
    ports = available_ports()
    for p in ports:
        if p["likely"] and "BLUETOOTH" not in p["hwid"].upper():
            return p["device"]
    for p in ports:
        if "BTHENUM" not in p["hwid"].upper():
            return p["device"]
    return ports[0]["device"] if ports else ""


def glob_ports() -> list[str]:                            # 兼容性小工具
    return sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*") + glob.glob("COM[0-9]*"))
