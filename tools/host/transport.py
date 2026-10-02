"""链路层：三种传输（真实串口 / 内置模拟器 / CSV 回放）+ 后台读线程。

UI 只跟 Link 打交道，所以"用真车还是用模拟器"对上层完全透明 ——
这也是没有硬件时能验证上位机的关键。
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Callable

import protocol as P

try:
    import serial
    from serial.tools import list_ports
    HAVE_PYSERIAL = True
except ImportError:                                   # pragma: no cover
    serial = None
    list_ports = None
    HAVE_PYSERIAL = False


# ================================================================ 传输

class Transport:
    """传输基类：读出原始字节、写入原始字节。"""

    name = "transport"

    def open(self) -> None: ...
    def close(self) -> None: ...
    def read(self, timeout: float = 0.05) -> bytes: return b""
    def write(self, data: bytes) -> int: return 0

    @property
    def is_open(self) -> bool:
        return False

    def describe(self) -> str:
        return self.name


def list_serial_ports() -> list[tuple[str, str]]:
    """返回 [(设备名, 描述)]。没有 pyserial 时返回空表。"""
    if not HAVE_PYSERIAL:
        return []
    out = []
    for p in list_ports.comports():
        desc = p.description or ""
        if p.hwid and p.hwid not in desc:
            desc = ("%s  %s" % (desc, p.hwid)).strip()
        out.append((p.device, desc))
    return sorted(out, key=lambda t: t[0])


class SerialTransport(Transport):
    """真实串口：无线的下载器（LPUART1 -> PC1=TX / PC0=RX）。"""

    def __init__(self, port: str, baud: int = 115200) -> None:
        if not HAVE_PYSERIAL:
            raise RuntimeError("未安装 pyserial，无法使用真实串口")
        self.port = port
        self.baud = baud
        self._ser = None

    def open(self) -> None:
        self._ser = serial.Serial(self.port, self.baud, timeout=0.02, write_timeout=0.5)

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            finally:
                self._ser = None

    def read(self, timeout: float = 0.05) -> bytes:
        if self._ser is None:
            return b""
        try:
            n = self._ser.in_waiting
        except OSError:
            return b""
        return self._ser.read(max(1, n)) if n else self._ser.read(1)

    def write(self, data: bytes) -> int:
        if self._ser is None:
            return 0
        return self._ser.write(data) or 0

    @property
    def is_open(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def describe(self) -> str:
        return "%s @ %d 8N1" % (self.port, self.baud)


class SimTransport(Transport):
    """内置模拟器：按 5ms 步进推进小车模型，并按遥测频率吐帧。

    写进来的下行帧会被模拟器处理，应答帧再回给上位机，
    所以 HELLO / 读参数 / 改参数 / 发命令整条通路都能离线验证。
    """

    name = "演示数据"

    def __init__(self, sim=None, speed: float = 1.0) -> None:
        from simulator import DemoSource
        self.sim = sim if sim is not None else DemoSource()
        self.speed = speed
        self._open = False
        self._last = 0.0
        self._acc = 0.0
        self._to_host = bytearray()
        self._parser = P.FrameParser()
        self._seq = 0

    def open(self) -> None:
        self._open = True
        self._last = time.monotonic()

    def close(self) -> None:
        self._open = False

    def read(self, timeout: float = 0.05) -> bytes:
        if not self._open:
            time.sleep(min(timeout, 0.02))
            return b""
        now = time.monotonic()
        dt = (now - self._last) * self.speed
        self._last = now
        dt = max(0.0, min(dt, 0.25))
        out = bytearray()
        # 固定 5ms 子步，物理更平滑；每子步最多发一帧
        steps = int(dt / 0.005)
        for _ in range(steps):
            self.sim.tick(0.005)
            if self.sim.due(0.005) > 0:
                out += P.build_telem_frame(self.sim.telemetry(), self._seq)
                self._seq = (self._seq + 1) & 0xFF
        if self._to_host:
            out += self._to_host
            del self._to_host[:]
        if not out:
            time.sleep(min(timeout, 0.005))
        return bytes(out)

    def write(self, data: bytes) -> int:
        for f in self._parser.feed(data):
            for resp in self.sim.handle(f):
                self._to_host += resp.to_bytes()
        return len(data)

    @property
    def is_open(self) -> bool:
        return self._open

    def describe(self) -> str:
        return "演示数据源（无硬件；只生成遥测信号，不做赛道仿真）"


class ReplayTransport(Transport):
    """CSV 回放：按录制时间轴把遥测重新吐出来。speed=0 表示尽快。"""

    name = "回放"

    def __init__(self, frames: list[P.Telemetry], speed: float = 1.0) -> None:
        self.frames = frames
        self.speed = speed
        self._open = False
        self._i = 0
        self._next_t = 0.0
        self._t0 = 0.0

    def open(self) -> None:
        self._open = True
        self._i = 0
        self._t0 = time.monotonic()
        self._next_t = self.frames[0].t_ms / 1000.0 if self.frames else 0.0

    def close(self) -> None:
        self._open = False

    def read(self, timeout: float = 0.05) -> bytes:
        if not self._open or self._i >= len(self.frames):
            time.sleep(min(timeout, 0.02))
            return b""
        out = bytearray()
        now = (time.monotonic() - self._t0) * (self.speed if self.speed > 0 else 1e9)
        while self._i < len(self.frames):
            t = self.frames[self._i]
            if self.speed > 0 and t.t_ms / 1000.0 > now:
                break
            out += P.build_telem_frame(t, self._i & 0xFF)
            self._i += 1
            if self.speed > 0 and len(out) > 64 * 1024:
                break
        if not out:
            time.sleep(min(timeout, 0.005))
        return bytes(out)

    def write(self, data: bytes) -> int:
        return len(data)                               # 回放模式不回应下行帧

    @property
    def is_open(self) -> bool:
        return self._open

    @property
    def finished(self) -> bool:
        return self._i >= len(self.frames)

    @property
    def progress(self) -> float:
        return self._i / len(self.frames) if self.frames else 1.0

    def describe(self) -> str:
        return "CSV 回放（%d 帧）" % len(self.frames)


class LoopbackTransport(Transport):
    """自测用：写进去的字节原样读回来。"""

    name = "回环"

    def __init__(self) -> None:
        self._buf = bytearray()
        self._open = False

    def open(self) -> None:
        self._open = True

    def close(self) -> None:
        self._open = False

    def read(self, timeout: float = 0.05) -> bytes:
        out = bytes(self._buf)
        self._buf.clear()
        if not out:
            time.sleep(min(timeout, 0.002))
        return out

    def write(self, data: bytes) -> int:
        self._buf += data
        return len(data)

    @property
    def is_open(self) -> bool:
        return self._open

    def describe(self) -> str:
        return "回环（自测）"


# ================================================================ 链路

@dataclass
class LinkStats:
    frames_in: int = 0
    frames_out: int = 0
    bytes_in: int = 0
    bytes_out: int = 0
    crc_errors: int = 0
    resyncs: int = 0
    length_errors: int = 0
    crc_errors_out: int = 0


class Link:
    """后台读线程 + 帧解析 + 统计。线程回调只往队列里塞，界面自己取。"""

    def __init__(self, transport: Transport,
                 on_frame: Callable[[P.Frame], None] | None = None,
                 on_error: Callable[[str], None] | None = None) -> None:
        self.transport = transport
        self.on_frame = on_frame
        self.on_error = on_error
        self.parser = P.FrameParser()
        self.stats = LinkStats()
        self._thread: threading.Thread | None = None
        self._run = False
        self._wlock = threading.Lock()
        self._seq = 0

    # --- 生命周期 ---
    def open(self) -> None:
        self.transport.open()
        self.parser.reset()
        self._run = True
        self._thread = threading.Thread(target=self._loop, name="host-link",
                                        daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._run = False
        t, self._thread = self._thread, None
        if t is not None and t.is_alive():
            t.join(timeout=0.6)
        try:
            self.transport.close()
        except Exception as exc:                       # pragma: no cover
            if self.on_error:
                self.on_error("关闭链路失败: %s" % exc)

    @property
    def is_open(self) -> bool:
        return self._run and self.transport.is_open

    def next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0xFF
        return self._seq

    def send(self, frame: P.Frame | bytes) -> bool:
        raw = frame.to_bytes() if isinstance(frame, P.Frame) else frame
        with self._wlock:
            try:
                n = self.transport.write(raw)
            except Exception as exc:
                if self.on_error:
                    self.on_error("发送失败: %s" % exc)
                return False
        self.stats.bytes_out += n
        self.stats.frames_out += 1
        return True

    # --- 读线程 ---
    def _loop(self) -> None:
        while self._run:
            try:
                data = self.transport.read(0.05)
            except Exception as exc:
                if self.on_error:
                    self.on_error("读取失败: %s" % exc)
                time.sleep(0.2)
                continue
            if not data:
                continue
            self.stats.bytes_in += len(data)
            for f in self.parser.feed(data):
                self.stats.frames_in += 1
                if self.on_frame:
                    try:
                        self.on_frame(f)
                    except Exception as exc:           # pragma: no cover
                        if self.on_error:
                            self.on_error("回调异常: %s" % exc)
            self._sync_parser_stats()

    def _sync_parser_stats(self) -> None:
        s = self.parser.stats
        self.stats.crc_errors = s.crc_errors
        self.stats.resyncs = s.resyncs
        self.stats.length_errors = s.length_errors
