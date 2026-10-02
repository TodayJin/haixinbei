"""演示数据源：没有真车时给界面喂一套遥测，方便**看波形、试调参**。

⚠ 这**不是赛道仿真**：没有场地、没有障碍、没有计分、没有循迹路线。
它只是个信号发生器，把几个通道按物理上说得通的关系生成出来：

* 循迹偏差按正弦摆动，且 **`line_kp` 越大摆幅越小** —— 改参数在波形上看得见；
* 8 路灰度掩码由偏差反推，所以灰度条会跟着动；
* 轮速 / PWM / 电流 / 电压互相关联；堵转时电流飙升、速度归零。

要真实数据就接串口；这个源只为"手边没车时界面能用、能试调参"。
"""

from __future__ import annotations

import math
import random
import struct
import time
from dataclasses import dataclass

import protocol as P

V_MAX_MMPS = 510.0
PWM_MAX = 7200.0
SENSOR_SPACING_CM = 1.2
SENSOR_OFFSETS = [(i - 3.5) * SENSOR_SPACING_CM for i in range(8)]
LINE_HALF_CM = (3.8 + 2.2) / 2.0


def wrap180(a: float) -> float:
    while a > 180.0:
        a -= 360.0
    while a < -180.0:
        a += 360.0
    return a


@dataclass
class DemoEvent:
    t: float
    kind: str
    value: float = 0.0
    note: str = ""


# 默认注入一个"堵转"用来演示告警与恢复
DEFAULT_EVENTS = [DemoEvent(14.0, "stuck", 1.5), DemoEvent(20.0, "release")]


class DemoSource:
    """确定性的演示信号源。seed 固定时行为可复现。"""

    def __init__(self, seed: int = 12345, telem_hz: int = 50,
                 events: list[DemoEvent] | None = None) -> None:
        self.rng = random.Random(seed)
        self.events = list(DEFAULT_EVENTS if events is None else events)
        self.telem_hz = telem_hz
        self.params: dict[int, float] = {p.pid: p.default for p in P.PARAMS}

        self.t = 0.0
        self.started = False
        self.logging = False
        self.stuck = False
        self.yaw = 0.0
        self.pitch = 0.0
        self.roll = 0.0
        self.line_err = 0.0
        self.line_mask = 0
        self.v_l = 0.0
        self.v_r = 0.0
        self.pwm_l = 0.0
        self.pwm_r = 0.0
        self.vbat = 12000.0
        self.ibat = 60.0
        self.servo1 = 1500
        self.servo2 = 1500
        self.flags = P.F_GYRO_OK
        self.state_id = 0
        self.loop_us = 210
        self._telem_accum = 0.0
        self._event_i = 0
        self._stuck_until = 0.0
        self._integral = 0.0

    def p(self, key: str) -> float:
        return self.params[P.PARAM_BY_KEY[key].pid]

    # ------------------------------------------------------------ 产出
    def tick(self, dt: float) -> None:
        self.t += dt
        self._run_events()
        if not self.started:
            self.v_l = self.v_r = self.pwm_l = self.pwm_r = 0.0
            self.ibat = 60.0 + self.rng.uniform(-3, 3)
            self.vbat += (12000.0 - self.vbat) * min(1.0, dt * 2)
            self.line_err = 0.0
            self.line_mask = 0
            self.flags = P.F_GYRO_OK | P.F_LINE_LOST
            return

        base = max(1.0, self.p("base_speed"))
        v = base / PWM_MAX * V_MAX_MMPS / 10.0          # cm/s
        kp = max(30.0, self.p("line_kp"))

        # 循迹偏差：正弦 + 噪声；Kp 越大摆幅越小（和真实闭环一个趋势）
        # 参考值用当前固件默认 2700（旧板 375 ×7.2），这样默认参数下的仿真动态不变。
        amp = max(0.25, 3.2 * (2700.0 / kp))
        target = amp * math.sin(2.0 * math.pi * self.t / 1.7) + self.rng.gauss(0, 0.12)
        ki = self.p("line_ki") / 10.8
        self._integral += (target - self.line_err) * dt * ki * 2.0
        self._integral = max(-4.0, min(4.0, self._integral))
        self.line_err += (target - self.line_err) * min(1.0, dt * 8.0)
        self.line_err = max(-4.0, min(4.0, self.line_err + self._integral * dt))

        # 灰度掩码由偏差反推（1 = 压线）
        centre = -self.line_err * SENSOR_SPACING_CM * 1.4
        mask = 0
        for i, off in enumerate(SENSOR_OFFSETS):
            if abs(off - centre) < LINE_HALF_CM:
                mask |= 1 << i
        self.line_mask = mask

        steer = self.line_err * 26.0 + self.rng.gauss(0, 1.5)
        self.yaw = wrap180(self.yaw + steer * dt * 0.9)
        self.pitch = 2.5 * math.sin(self.t * 0.6)
        self.roll = 1.5 * math.sin(self.t * 2.1) + self.rng.gauss(0, 0.4)

        if self.stuck:
            v = 0.0
            self.ibat = self.p("stall_current") * 1.2
        else:
            self.ibat = 420.0 + v * 90.0 + abs(steer) * 2.0 + self.rng.gauss(0, 15)
        self.vbat += ((self.p("vbat_nominal") - self.ibat * 0.35) - self.vbat) * min(1.0, dt * 1.5)

        diff = steer * 1.1
        self.v_l = max(-1500.0, min(1500.0, v * 10.0 - diff))
        self.v_r = max(-1500.0, min(1500.0, v * 10.0 + diff))
        self.pwm_l = max(-PWM_MAX, min(PWM_MAX, self.v_l / V_MAX_MMPS * PWM_MAX))
        self.pwm_r = max(-PWM_MAX, min(PWM_MAX, self.v_r / V_MAX_MMPS * PWM_MAX))
        self.servo1 = int(1500 + 400 * math.sin(self.t * 0.7))
        self.servo2 = int(1500 + 300 * math.cos(self.t * 0.5))
        self.loop_us = int(205 + abs(steer) * 0.6 + self.rng.gauss(0, 4))

        self.flags = P.F_GYRO_OK
        if self.logging:
            self.flags |= P.F_LOGGING
        if self.stuck:
            self.flags |= P.F_STUCK
        if mask == 0:
            self.flags |= P.F_LINE_LOST

    def telemetry(self) -> P.Telemetry:
        return P.Telemetry(
            t_ms=int(self.t * 1000) & 0xFFFFFFFF,
            yaw_x10=max(-32768, min(32767, int(round(self.yaw * 10)))),
            pitch_x10=int(self.pitch * 10),
            roll_x10=int(self.roll * 10),
            v_l_mmps=int(self.v_l), v_r_mmps=int(self.v_r),
            pwm_l=int(self.pwm_l), pwm_r=int(self.pwm_r),
            vbat_mv=int(self.vbat), ibat_ma=int(self.ibat),
            servo1_us=int(self.servo1), servo2_us=int(self.servo2),
            line_mask=self.line_mask, state_id=self.state_id,
            loop_us=max(0, min(65535, self.loop_us)), flags=self.flags,
        )

    def due(self, dt: float) -> int:
        self._telem_accum += dt * self.telem_hz
        n = int(self._telem_accum)
        self._telem_accum -= n
        return min(n, 5)

    def _run_events(self) -> None:
        while self._event_i < len(self.events) and self.t >= self.events[self._event_i].t:
            ev = self.events[self._event_i]
            self._event_i += 1
            if ev.kind == "stuck":
                self.stuck = True
                self._stuck_until = self.t + ev.value
            elif ev.kind == "release":
                self.stuck = False
        if self._stuck_until and self.t >= self._stuck_until:
            self.stuck = False
            self._stuck_until = 0.0

    # ------------------------------------------------------------ 收帧
    def handle(self, frame: P.Frame) -> list[P.Frame]:
        out: list[P.Frame] = []
        if frame.type == P.T_HELLO_REQ:
            out.append(P.Frame(P.T_HELLO, P.Hello(
                fw_version=0x0001, proto_version=P.PROTO_VERSION,
                param_crc=P.param_table_crc(), telem_hz=self.telem_hz,
                name="demo-signal").to_bytes(), frame.seq))
        elif frame.type == P.T_PARAM_GET:
            pid = struct.unpack("<H", frame.payload)[0]
            if pid == 0xFFFF:
                for spec in P.PARAMS:
                    out.append(P.Frame(P.T_PARAM_REPORT,
                                       struct.pack("<Hf", spec.pid, self.params[spec.pid]),
                                       frame.seq))
            elif pid in self.params:
                out.append(P.Frame(P.T_PARAM_REPORT,
                                   struct.pack("<Hf", pid, self.params[pid]), frame.seq))
            else:
                out.append(P.Frame(P.T_ACK, bytes([frame.seq, P.ST_BAD_PARAM]), frame.seq))
        elif frame.type == P.T_PARAM_SET:
            pid, val = struct.unpack("<Hf", frame.payload)
            spec = P.PARAM_BY_ID.get(pid)
            if spec is None:
                out.append(P.Frame(P.T_ACK, bytes([frame.seq, P.ST_BAD_PARAM]), frame.seq))
            elif not (spec.vmin <= val <= spec.vmax):
                out.append(P.Frame(P.T_ACK, bytes([frame.seq, P.ST_OUT_OF_RANGE]), frame.seq))
            else:
                self.params[pid] = val
                out.append(P.Frame(P.T_ACK, bytes([frame.seq, P.ST_OK]), frame.seq))
                out.append(P.Frame(P.T_PARAM_REPORT, struct.pack("<Hf", pid, val), frame.seq))
        elif frame.type == P.T_PARAM_SAVE:
            out.append(P.Frame(P.T_ACK, bytes([frame.seq, P.ST_OK]), frame.seq))
        elif frame.type == P.T_CMD:
            cmd, arg = struct.unpack("<Bi", frame.payload)
            out.append(P.Frame(P.T_ACK, bytes([frame.seq, self._do_cmd(cmd, arg)]), frame.seq))
        else:
            out.append(P.Frame(P.T_ACK, bytes([frame.seq, P.ST_UNKNOWN_TYPE]), frame.seq))
        return out

    def _do_cmd(self, cmd: int, arg: int) -> int:
        if cmd == P.CMD_STOP:
            self.started = False
            self.state_id = 0
            return P.ST_OK
        if cmd in (P.CMD_START_A, P.CMD_START_B, P.CMD_START_C):
            self.started = True
            self.t = 0.0
            self._event_i = 0
            self.stuck = False
            self._integral = 0.0
            return P.ST_OK
        if cmd == P.CMD_CALIB_GYRO:
            self.yaw = 0.0
            return P.ST_OK
        if cmd == P.CMD_SAVE_PARAMS:
            return P.ST_OK
        if cmd == P.CMD_CLEAR_FAULT:
            self.stuck = False
            return P.ST_OK
        if cmd == P.CMD_SET_TELEM_HZ:
            self.telem_hz = max(1, min(200, arg))
            return P.ST_OK
        if cmd == P.CMD_SET_LOGGING:
            self.logging = bool(arg)
            return P.ST_OK
        if cmd == P.CMD_RESET:
            self.__init__(seed=self.rng.randint(0, 1 << 30))
            return P.ST_OK
        return P.ST_BAD_CMD
