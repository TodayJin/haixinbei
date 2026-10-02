"""会话层：把协议包成"一台设备"。

界面只调用 Device，不直接碰字节。它负责：
  * 连接/断开（真实串口、内置模拟器、CSV 回放三种模式）
  * 遥测订阅 + 滚动历史（给波形和仪表用）
  * 参数读写与本地缓存（含范围检查、预设存取）
  * 命令下发与 ACK 匹配
  * 录制到 CSV
"""

from __future__ import annotations

import json
import os
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Callable

import protocol as P
import recorder as R
from transport import (Link, LoopbackTransport, ReplayTransport, SerialTransport,
                       SimTransport, Transport)

HISTORY_LEN = 4000          # 50Hz 下约 80 秒


@dataclass
class DeviceState:
    connected: bool = False
    mode: str = "未连接"
    frames: int = 0
    crc_errors: int = 0
    last_rx: float = 0.0
    telem_hz: float = 0.0
    param_crc_ok: bool | None = None
    board_name: str = ""


class Device:
    """一台车。所有回调都在读线程里触发，界面自己丢进队列。"""

    def __init__(self, history_len: int = HISTORY_LEN) -> None:
        self.link: Link | None = None
        self.transport: Transport | None = None
        self.state = DeviceState()
        self.last: P.Telemetry | None = None
        self.hello: P.Hello | None = None
        self.params: dict[int, float] = {p.pid: p.default for p in P.PARAMS}
        self._subs: dict[str, list[Callable]] = {}
        self._lock = threading.RLock()
        self._pending: dict[int, tuple[str, float]] = {}
        self._t_last_rate = 0.0
        self._n_at_rate = 0
        self.recorder: R.CsvRecorder | None = None

        self.hist_t: deque[float] = deque(maxlen=history_len)
        self.hist: dict[str, deque[float]] = {
            k: deque(maxlen=history_len)
            for k in ("yaw", "v_l", "v_r", "pwm_l", "pwm_r", "vbat", "ibat",
                      "line_err", "loop_us")
        }

    # ------------------------------------------------------------ 订阅
    def subscribe(self, event: str, fn: Callable) -> None:
        self._subs.setdefault(event, []).append(fn)

    def _emit(self, event: str, *args) -> None:
        for fn in self._subs.get(event, ()):  # type: ignore[arg-type]
            fn(*args)

    # ------------------------------------------------------------ 连接
    def open_sim(self, speed: float = 1.0) -> None:
        self._open(SimTransport(speed=speed), "演示数据")

    def open_serial(self, port: str, baud: int = 115200) -> None:
        self._open(SerialTransport(port, baud), "串口 %s" % port)

    def open_replay(self, path: str, speed: float = 1.0) -> None:
        frames = R.read_csv(path)
        self._open(ReplayTransport(frames, speed=speed), "回放 %s" % os.path.basename(path))

    def open_loopback(self) -> None:
        self._open(LoopbackTransport(), "回环")

    def _open(self, transport: Transport, mode: str) -> None:
        self.close()
        self.transport = transport
        self.state = DeviceState(connected=True, mode=mode)
        self.link = Link(transport, on_frame=self._on_frame,
                         on_error=lambda m: self._emit("status", "错误：%s" % m))
        self.link.open()
        self.hello = None
        self.state.param_crc_ok = None
        self._emit("status", "已连接（%s）" % transport.describe())
        self.request_hello()
        self.get_all_params()

    def close(self) -> None:
        if self.recorder is not None:
            self.stop_recording()
        if self.link is not None:
            self.link.close()
            self.link = None
        self.transport = None
        if self.state.connected:
            self.state.connected = False
            self._emit("status", "已断开")

    @property
    def connected(self) -> bool:
        return bool(self.link and self.link.is_open)

    # ------------------------------------------------------------ 收帧
    def _on_frame(self, f: P.Frame) -> None:
        try:
            if f.type == P.T_TELEM:
                self._on_telem(P.Telemetry.from_bytes(f.payload))
            elif f.type == P.T_PARAM_REPORT:
                pid, val = P.parse_param_report(f.payload)
                self.params[pid] = val
                self._emit("param", pid, val)
            elif f.type == P.T_ACK:
                seq, st = P.parse_ack(f.payload)
                desc = self._pending.pop(seq, ("未知请求", 0.0))[0]
                self._emit("ack", desc, st)
                self._emit("status", "应答：%s -> %s" % (desc, P.STATUS_NAMES.get(st, st)))
            elif f.type == P.T_HELLO:
                self.hello = P.Hello.from_bytes(f.payload)
                self.state.board_name = self.hello.name
                self.state.telem_hz = float(self.hello.telem_hz)
                local = P.param_table_crc()
                self.state.param_crc_ok = (self.hello.param_crc == local)
                if not self.state.param_crc_ok:
                    self._emit("status", "警告：参数表指纹不一致（板 0x%04X / 本机 0x%04X），"
                                         "参数名可能对不上" % (self.hello.param_crc, local))
                else:
                    self._emit("status", "握手成功：%s，固件 v%d，遥测 %dHz"
                               % (self.hello.name, self.hello.fw_version, self.hello.telem_hz))
                self._emit("hello", self.hello)
            elif f.type == P.T_TEXT:
                self._emit("text", P.decode_text(f.payload))
            elif f.type == P.T_LOG:
                self._emit("log", f.payload)
        except Exception as exc:
            self._emit("status", "解帧异常（已忽略）：%s" % exc)

    def _on_telem(self, t: P.Telemetry) -> None:
        now = time.monotonic()
        with self._lock:
            self.last = t
            self.state.frames += 1
            self.state.last_rx = now
            self.hist_t.append(now)
            self.hist["yaw"].append(t.yaw)
            self.hist["v_l"].append(float(t.v_l_mmps))
            self.hist["v_r"].append(float(t.v_r_mmps))
            self.hist["pwm_l"].append(float(t.pwm_l))
            self.hist["pwm_r"].append(float(t.pwm_r))
            self.hist["vbat"].append(t.vbat)
            self.hist["ibat"].append(t.ibat)
            self.hist["line_err"].append(t.line_error)
            self.hist["loop_us"].append(float(t.loop_us))
            if self.recorder is not None:
                self.recorder.write(t)
            # 实测帧率（每秒算一次）
            self._n_at_rate += 1
            if now - self._t_last_rate >= 1.0:
                dt = now - self._t_last_rate
                self.state.telem_hz = self._n_at_rate / dt
                self._t_last_rate = now
                self._n_at_rate = 0
        self._emit("telemetry", t)

    def snapshot(self) -> tuple[list[float], dict[str, list[float]]]:
        """给界面画波形用的一致快照。"""
        with self._lock:
            return list(self.hist_t), {k: list(v) for k, v in self.hist.items()}

    # ------------------------------------------------------------ 发包
    def _send(self, ftype: int, payload: bytes, desc: str) -> bool:
        """组帧 + 分配序号 + 记账（用于 ACK 匹配）。"""
        if self.link is None or not self.link.is_open:
            self._emit("status", "未连接，无法发送：%s" % desc)
            return False
        seq = self.link.next_seq()
        self._pending[seq] = (desc, time.monotonic())
        if len(self._pending) > 256:                   # 防止长期无应答堆积
            oldest = min(self._pending, key=lambda k: self._pending[k][1])
            self._pending.pop(oldest, None)
        return self.link.send(P.Frame(ftype, payload, seq))

    def request_hello(self) -> bool:
        return self._send(P.T_HELLO_REQ, b"", "握手")

    def get_all_params(self) -> bool:
        return self._send(P.T_PARAM_GET, struct.pack("<H", 0xFFFF), "读取全部参数")

    def get_param(self, pid: int) -> bool:
        spec = P.PARAM_BY_ID.get(pid)
        name = spec.name if spec else hex(pid)
        return self._send(P.T_PARAM_GET, struct.pack("<H", pid), "读取参数 %s" % name)

    def set_param(self, key: str, value: float) -> tuple[bool, str]:
        """下发一个参数。返回 (是否发出, 提示文本)。超范围会在本地就拦下。"""
        spec = P.PARAM_BY_KEY.get(key)
        if spec is None:
            return False, "未知参数 %s" % key
        if not (spec.vmin <= value <= spec.vmax):
            return False, "%s 超范围（%.3g ~ %.3g）" % (spec.name, spec.vmin, spec.vmax)
        ok = self._send(P.T_PARAM_SET, struct.pack("<Hf", spec.pid, float(value)),
                        "设置 %s = %.4g" % (spec.name, value))
        if ok:
            self.params[spec.pid] = float(value)       # 乐观更新，回读会覆盖
        return ok, "" if ok else "发送失败"

    def set_param_pid(self, pid: int, value: float) -> bool:
        spec = P.PARAM_BY_ID.get(pid)
        if spec is None:
            return False
        return self._send(P.T_PARAM_SET, struct.pack("<Hf", pid, float(value)),
                          "设置 %s = %.4g" % (spec.name, value))

    def save_params(self) -> bool:
        return self._send(P.T_PARAM_SAVE, b"", "参数写 Flash")

    def send_cmd(self, cmd: int, arg: int = 0) -> bool:
        return self._send(P.T_CMD, struct.pack("<Bi", cmd, int(arg)),
                          P.CMD_NAMES.get(cmd, "命令 0x%02X" % cmd))

    def set_telem_hz(self, hz: int) -> bool:
        return self._send(P.T_CMD, struct.pack("<Bi", P.CMD_SET_TELEM_HZ, int(hz)),
                          "遥测频率 -> %dHz" % hz)

    # ------------------------------------------------------------ 参数快照
    def param_snapshot(self) -> list[tuple[P.ParamSpec, float]]:
        return [(spec, self.params.get(spec.pid, spec.default)) for spec in P.PARAMS]

    def to_preset(self) -> dict:
        return {
            "format": "haixin-host-preset",
            "version": 1,
            "saved": time.strftime("%Y-%m-%d %H:%M:%S"),
            "param_crc": P.param_table_crc(),
            "values": {spec.key: self.params.get(spec.pid, spec.default) for spec in P.PARAMS},
        }

    def save_preset(self, path: str) -> None:
        directory = os.path.dirname(os.path.abspath(path))
        if directory:
            os.makedirs(directory, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(self.to_preset(), fh, ensure_ascii=False, indent=2)

    def load_preset(self, path: str) -> tuple[int, list[str]]:
        """读预设进本地缓存（不自动下发，由界面决定）。返回 (成功数, 警告)。"""
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        warn: list[str] = []
        if data.get("param_crc") not in (None, P.param_table_crc()):
            warn.append("预设的参数表指纹与本机不一致，可能来自旧版本")
        n = 0
        for key, val in (data.get("values") or {}).items():
            spec = P.PARAM_BY_KEY.get(key)
            if spec is None:
                warn.append("预设里有未知参数 %s，已跳过" % key)
                continue
            try:
                fval = float(val)
            except (TypeError, ValueError):
                warn.append("参数 %s 的值不是数字，已跳过" % key)
                continue
            self.params[spec.pid] = fval
            n += 1
        return n, warn

    def push_all_params(self) -> int:
        """把本地缓存里的参数全部下发给车。"""
        n = 0
        for spec in P.PARAMS:
            if self.set_param_pid(spec.pid, self.params.get(spec.pid, spec.default)):
                n += 1
        return n

    # ------------------------------------------------------------ 录制
    def start_recording(self, path: str) -> str:
        if self.recorder is not None:
            self.stop_recording()
        self.recorder = R.CsvRecorder(path)
        self._emit("status", "开始录制 -> %s" % path)
        return path

    def stop_recording(self) -> int:
        n = 0
        if self.recorder is not None:
            n = self.recorder.count
            self.recorder.close()
            self.recorder = None
            self._emit("status", "录制结束，共 %d 帧" % n)
        return n

    @property
    def recording(self) -> bool:
        return self.recorder is not None

    # ------------------------------------------------------------ 统计
    def link_stats_text(self) -> str:
        if self.link is None:
            return "未连接"
        s = self.link.stats
        return ("收 %d 帧 / %d B，发 %d 帧；CRC 错 %d，重同步 %d"
                % (s.frames_in, s.bytes_in, s.frames_out,
                   s.crc_errors, s.resyncs))
