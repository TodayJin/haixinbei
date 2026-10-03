"""内置仿真信号源 —— `--sim` 模式（无车也能把整条链路跑起来，见 docs/API.md §6）。

仿真车实现了**同一套帧协议**（用 protocol.build_frame 组帧、再经 FrameDecoder 解回来，
所以它同时也在测协议层），行为上模仿真车：
  * 50Hz 遥测（yaw/pitch/roll、灰度 8 路、电池、舵机角度、模式/任务号/flags）；
  * 偶发丢线（`flags.lost`，灰度全白、重心冻结）；
  * 参数表 43 项可读可写（含范围校验，越界回 ST_OUT_OF_RANGE）；
  * 动作表 14 项、舵机两路、驾驶模式（wheels/base_turn/trace/off）+ 500ms 看门狗；
  * nvram 保存会走 待保存 → 保存中 → 已保存 的状态变化。
"""
from __future__ import annotations

import math
import random

from . import params as P
from . import protocol as pr

# 与 firmware/App/Bsp/servo.c 的 k_action_default 逐行一致（动作号从 1 开始）。
# 仿真车代表"刚上电、还没改过动作"的车（Servo_Init() 会调 Servo_ResetActionPoses()
# 把运行表复位成这张出厂表），所以初始时 14 个动作都不脏。
ACTION_NAMES = ["收回", "打下准备", "打下1", "打下2", "台阶2收回", "大坡1", "三级12",
                "三级2收回", "三级3打", "高台", "高台下", "收回2",
                "打下1(另一路)", "打下准备(另一路)"]
ACTION_DEFAULT = [(0, 0), (60, 120), (105, 120), (135, 150), (80, 170), (100, 120), (95, 120),
                  (40, 10), (100, 80), (115, 120), (85, 120), (40, 0), (90, 60), (30, 60)]
COMPOUND = {5: "135/170 →0.4s→ 90 →0.1s→ 80", 9: "70/10 →0.2s→ 100/50 →0.1s→ 100/80"}


def servo_deg_to_us(deg: float) -> int:
    """与固件 Servo_SetAngleDev 一致：500µs + 2000µs*angle/180。"""
    return int(round(500.0 + 2000.0 * max(0.0, min(180.0, deg)) / 180.0))


class SimCar:
    def __init__(self, seed: int = 7):
        self.rng = random.Random(seed)
        self.t0 = 0
        self.now = 0
        self.hz = 50
        self.next_tx = 0
        self.params = dict(P.DEFAULTS)
        self.actions = [list(a) for a in ACTION_DEFAULT]
        self.servo_deg = [0.0, 0.0]
        self.servo_us = [servo_deg_to_us(0.0), servo_deg_to_us(0.0)]
        self.deviation = [0.0, 0.0]
        self.action = 0
        self.nvram = 0
        self.nvram_ms = 0
        self.pending: list[pr.Frame] = []
        self.drive_mode = 0
        self.drive_enabled = False
        self.drive_last_ms = 0
        self.drive_trips = 0
        self.vl_t = 0.0
        self.vr_t = 0.0
        self.pwm_l = 0.0
        self.pwm_r = 0.0
        self.yaw = -64.7
        self.yaw_ref = -64.7
        self.pitch = 5.8
        self.roll = 4.7
        self.vbat = 12010
        self.ibat = 330
        self.loop_us = 412
        self.mode = 0
        self.renwu = 0
        self.state_id = 23
        self.lost = False
        self.lost_until = 0
        self.track = 3.6                      # 虚拟线上的位置（格）
        self.cal = None
        self._build_cal()

    # ---------------------------------------------------------- 标定假数据
    def _build_cal(self):
        ch = []
        for i in range(8):
            blk = 3600 + i * 40
            wht = 900 + i * 15
            ch.append({"i": i, "blk": blk, "wht": wht, "thr": (blk + wht) // 2,
                       "jitter": 8 + i, "pol": 0, "bad": 0, "ok": True, "reason": 0,
                       "level": 0, "thr_level": 0})
        self.cal = {"active_set": 0, "quality": 8, "ch": ch}

    # ---------------------------------------------------------- 对外
    def start(self, now_ms: int):
        self.t0 = now_ms
        self.now = now_ms

    def send(self, data: bytes):
        """收上位机发来的原始字节（内部自己解帧，模拟真车的 rx_byte）。"""
        dec = pr.FrameDecoder()
        for f in dec.feed(data):
            self._handle(f)

    def poll(self, now_ms: int) -> list[pr.Frame]:
        self.now = now_ms
        self._physics(now_ms)
        out: list[pr.Frame] = []
        if self.pending:
            out.extend(self.pending)
            self.pending = []
        if self.hz and now_ms >= self.next_tx:
            self.next_tx = now_ms + max(1, int(1000 / self.hz))
            out.append(pr.Frame(pr.T_TELEM2, 0, self._telem_bytes()))
        return out

    def _telem(self) -> dict:
        gray = self._gray()
        lm = 0
        for i, g in enumerate(gray):
            if g > 128:
                lm |= 1 << i
        return {
            "t": self.now - self.t0,
            "yaw": self.yaw, "pitch": self.pitch, "roll": self.roll,
            "vl": int(round(self.vl_t)), "vr": int(round(self.vr_t)),
            "pwm_l": int(round(self.pwm_l)), "pwm_r": int(round(self.pwm_r)),
            "vbat_mv": int(self.vbat), "ibat_ma": int(self.ibat),
            "servo1_us": self.servo_us[0], "servo2_us": self.servo_us[1],
            "line": [(lm >> i) & 1 for i in range(8)], "gray": gray,
            "state_id": self.state_id, "loop_us": self.loop_us,
            "mode": self.mode, "renwu": self.renwu, "action": self.action,
            "nvram": self.nvram, "cross": False,
            "trace": {"pos": 0.0 if self.lost else round(self.track - 3.5, 2),
                      "pid": 0 if self.lost else int(round((self.track - 3.5) * 300))},
            "angle": {"err": round(self.yaw_ref - self.yaw, 2),
                      "rate_ref": 0.0, "corr": int(round(self.pwm_l - self.pwm_r)) // 2},
            "flags": {"gyro_ok": True, "lost": self.lost, "slope": abs(self.pitch) > 8.0,
                      "host_drive": self.drive_enabled, "motor": abs(self.pwm_l) > 50,
                      "logging": False, "flash": self.nvram == 2,
                      "fault": self.drive_trips > 3},
            "drive": {"mode": self.drive_mode, "enabled": self.drive_enabled},
            "watchdog_trips": min(255, self.drive_trips),
            "servo1_deg": int(round(self.servo_deg[0])),
            "servo2_deg": int(round(self.servo_deg[1])),
            "servo_dev": {"1": int(self.deviation[0]), "2": int(self.deviation[1])},
        }

    def _telem_bytes(self) -> bytes:
        return pr.encode_telemetry2(self._telem())

    def _gray(self) -> list[int]:
        if self.lost:
            return [self.rng.randint(0, 12) for _ in range(8)]
        out = []
        for i in range(8):
            d = abs(i - self.track)
            out.append(int(max(6, min(255, 240 - 90 * d))))
        return out

    # ---------------------------------------------------------- 物理
    def _physics(self, now_ms: int):
        # 看门狗：500ms 没收到合法帧就停车
        if self.drive_enabled and now_ms - self.drive_last_ms > 500:
            self.drive_enabled = False
            self.drive_mode = 0
            self.drive_trips += 1
            self.vl_t = self.vr_t = 0.0

        # 舵机缓动（与固件一样 20ms 一步、最小步 0.5°）
        for i in (0, 1):
            self.servo_us[i] = servo_deg_to_us(self.servo_deg[i]) + int(self.deviation[i])

        # 转向 → 航向积分；左右轮差速 → 也带一点转向
        diff = (self.pwm_r - self.pwm_l) / 7200.0
        self.yaw += diff * 3.0 + math.sin(now_ms / 900.0) * 0.05
        if self.mode == 2:                      # 循迹：虚拟线跟着重心走
            self.track += (3.5 - self.track) * 0.06 + self.rng.gauss(0, 0.02)
            self.rng.random()
        if now_ms > self.lost_until and self.rng.random() < 0.002:
            self.lost = True
            self.lost_until = now_ms + self.rng.randint(200, 600)
        if self.lost and now_ms > self.lost_until:
            self.lost = False
        self.pwm_l += (self.vl_t * 0.9 + 500.0 * (1 if self.vl_t > 0 else 0) - self.pwm_l) * 0.05
        self.pwm_r += (self.vr_t * 0.9 + 500.0 * (1 if self.vr_t > 0 else 0) - self.pwm_r) * 0.05
        self.vbat += self.rng.gauss(0, 3)
        self.ibat = 300 + abs(self.pwm_l + self.pwm_r) / 20.0
        self.pitch = 5.8 + math.sin(now_ms / 700.0) * 1.2
        self.roll = 4.7 + math.cos(now_ms / 500.0) * 0.8
        self.loop_us = 400 + int(self.rng.gauss(0, 25))
        if self.nvram == 1 and now_ms - self.nvram_ms > 1500:
            self.nvram = 2
            self.nvram_ms = now_ms
        elif self.nvram == 2 and now_ms - self.nvram_ms > 120:
            self.nvram = 3
            self.nvram_ms = now_ms

    def _ack(self, seq: int, st: int):
        self.pending.append(pr.Frame(pr.T_ACK, seq, bytes([seq & 0xFF, st & 0xFF])))

    def _text(self, s: str):
        self.pending.append(pr.Frame(pr.T_TEXT, 0, s.encode("utf-8")))

    def _actions_frame(self, seq: int = 0):
        pl = bytearray()
        for a1, a2 in self.actions:
            pl += bytes([a1 & 0xFF, a2 & 0xFF])
        self.pending.append(pr.Frame(pr.T_ACTIONS, seq, bytes(pl)))

    def _drive_frame(self, seq: int = 0):
        p = bytes([self.drive_mode, 1 if self.drive_enabled else 0]) \
            + pr.pack_i16(int(self.vl_t)) + pr.pack_i16(int(self.vr_t)) \
            + pr.pack_i16(int(self.pwm_l)) + pr.pack_i16(int(self.pwm_r)) \
            + bytes([int(self.servo_deg[0]) & 0xFF, int(self.servo_deg[1]) & 0xFF])
        self.pending.append(pr.Frame(pr.T_DRIVE_REPORT, seq, p))

    def _cal_frame(self, seq: int):
        p = bytearray(82)
        p[0] = 0xFF
        for i, c in enumerate(self.cal["ch"]):
            p[1 + i] = c["pol"]
            p[9 + i] = c["bad"]
            p[18 + i * 2:20 + i * 2] = pr.pack_u16(c["blk"])
            p[34 + i * 2:36 + i * 2] = pr.pack_u16(c["wht"])
            p[50 + i * 2:52 + i * 2] = pr.pack_u16(c["thr"])
            p[66 + i * 2:68 + i * 2] = pr.pack_u16(c["jitter"])
        self.pending.append(pr.Frame(pr.T_CAL_REPORT, seq, bytes(p)))

    def _hello(self, seq: int):
        p = bytearray(24)
        p[0:2] = pr.pack_u16(0x0002)
        p[2] = 2
        p[4:6] = pr.pack_u16(_param_crc())
        p[6:8] = pr.pack_u16(self.hz)
        name = b"quandi-car"
        p[8:8 + len(name)] = name
        p[20:24] = bytes([0x7F, 0, 0, 0])
        self.pending.append(pr.Frame(pr.T_HELLO, seq, bytes(p)))

    def _param_items(self):
        return [(p.id, self.params.get(p.id, p.default)) for p in P.PARAMS]

    def _handle(self, f: pr.Frame):
        self.drive_last_ms = self.now            # 任何合法帧都喂驾驶看门狗
        if f.type == pr.T_HELLO_REQ:
            self._hello(f.seq)
        elif f.type == pr.T_PARAM_GET:
            pid = pr.u16(f.payload, 0) if len(f.payload) >= 2 else 0xFFFF
            if pid == 0xFFFF:
                for i, v in self._param_items():
                    self.pending.append(pr.Frame(pr.T_PARAM_REPORT, f.seq, pr.pack_u16(i) + pr.pack_f32(v)))
            else:
                spec = P.BY_ID.get(pid)
                if spec is None:
                    self._ack(f.seq, pr.ST_BAD_PARAM)
                else:
                    self.pending.append(pr.Frame(pr.T_PARAM_REPORT, f.seq,
                                                 pr.pack_u16(pid) + pr.pack_f32(self.params.get(pid, spec.default))))
        elif f.type == pr.T_PARAM_SET:
            if len(f.payload) < 6:
                self._ack(f.seq, pr.ST_BAD_PARAM)
                return
            pid, v = pr.decode_param_report(f.payload)
            spec = P.BY_ID.get(pid)
            if spec is None:
                self._ack(f.seq, pr.ST_BAD_PARAM)
            elif not (v >= spec.min and v <= spec.max):
                self._ack(f.seq, pr.ST_OUT_OF_RANGE)
            else:
                self.params[pid] = v
                self._ack(f.seq, pr.ST_OK)
                self.pending.append(pr.Frame(pr.T_PARAM_REPORT, f.seq,
                                             pr.pack_u16(pid) + pr.pack_f32(v)))
        elif f.type == pr.T_PARAM_SAVE:
            self._ack(f.seq, pr.ST_OK)
            self._text("[sim] param save queued (writes when idle)")
            self.nvram = 1
            self.nvram_ms = self.now
        elif f.type == pr.T_CAL_GET:
            self._cal_frame(f.seq)
        elif f.type == pr.T_ACTIONS_GET:
            self._actions_frame(f.seq)
        elif f.type == pr.T_ACTION_SET:
            if len(f.payload) < 3:
                self._ack(f.seq, pr.ST_BAD_PARAM)
                return
            n, a1, a2 = f.payload[0], f.payload[1], f.payload[2]
            if not (1 <= n <= len(self.actions)) or a1 > 180 or a2 > 180:
                self._ack(f.seq, pr.ST_OUT_OF_RANGE)
                return
            self.actions[n - 1] = [a1, a2]
            self._servo_apply(n)
            self._ack(f.seq, pr.ST_OK)
            self._actions_frame(f.seq)
        elif f.type == pr.T_ACTION_SAVE:
            self._ack(f.seq, pr.ST_OK)
            self._text("[sim] action table queued for flash")
            self.nvram = 1
            self.nvram_ms = self.now
        elif f.type == pr.T_SERVO_SET:
            if len(f.payload) < 3:
                self._ack(f.seq, pr.ST_BAD_PARAM)
                return
            ch = f.payload[0]
            deg = pr.i16(f.payload, 1)
            if ch not in (1, 2):
                self._ack(f.seq, pr.ST_BAD_PARAM)
            elif not (0 <= deg <= 180):
                self._ack(f.seq, pr.ST_OUT_OF_RANGE)
            else:
                self.servo_deg[ch - 1] = float(deg)
                self._ack(f.seq, pr.ST_OK)
        elif f.type == pr.T_SERVO_PLAY:
            n = f.payload[0] if f.payload else 0
            if n == 0:
                self.servo_deg = [0.0, 0.0]
                self.action = 0
                self._ack(f.seq, pr.ST_OK)
            elif 1 <= n <= len(self.actions):
                self._servo_apply(n)
                self._ack(f.seq, pr.ST_OK)
            else:
                self._ack(f.seq, pr.ST_OUT_OF_RANGE)
        elif f.type == pr.T_SERVO_DEV:
            if len(f.payload) < 3:
                self._ack(f.seq, pr.ST_BAD_PARAM)
                return
            ch = f.payload[0]
            dev = pr.i16(f.payload, 1)
            if ch not in (1, 2):
                self._ack(f.seq, pr.ST_BAD_PARAM)
            elif not (-100 <= dev <= 100):
                self._ack(f.seq, pr.ST_OUT_OF_RANGE)
            else:
                self.deviation[ch - 1] = float(dev)
                self._ack(f.seq, pr.ST_OK)
        elif f.type == pr.T_DRIVE:
            if len(f.payload) < 5:
                self._ack(f.seq, pr.ST_BAD_PARAM)
                self._drive_frame(f.seq)
                return
            mode = f.payload[0]
            a = pr.i16(f.payload, 1)
            b = pr.i16(f.payload, 3)
            if self.renwu:
                self._ack(f.seq, pr.ST_NOT_READY)
                self._text("[sim] drive refused: task running")
            elif mode not in (0, 1, 2, 3):
                self._ack(f.seq, pr.ST_BAD_PARAM)
            else:
                self.drive_mode = mode
                self.drive_enabled = mode != 0
                if mode == 1:
                    self.vl_t, self.vr_t = float(a), float(b)
                    self.mode = 0
                elif mode == 2:
                    self.mode = 1
                    self.yaw_ref = self.yaw + b
                elif mode == 3:
                    self.mode = 3
                else:
                    self.vl_t = self.vr_t = 0.0
                    self.mode = 0
                self._ack(f.seq, pr.ST_OK)
            self._drive_frame(f.seq)
        elif f.type == pr.T_DRIVE_REPORT:
            self._drive_frame(f.seq)
        elif f.type == pr.T_CMD:
            cmd = f.payload[0] if f.payload else 0
            arg = f.payload[1] if len(f.payload) > 1 else 0
            if cmd == pr.CMD_SET_TELEM_HZ:
                self.hz = max(1, min(200, arg))
                self._ack(f.seq, pr.ST_OK)
            elif cmd == pr.CMD_SET_TELEM_MODE:
                self._ack(f.seq, pr.ST_OK)
            elif cmd == pr.CMD_STOP:
                self.drive_enabled = False
                self.drive_mode = 0
                self.vl_t = self.vr_t = 0.0
                self.mode = 0
                self.renwu = 0
                self._ack(f.seq, pr.ST_OK)
            elif cmd == pr.CMD_START_A:
                self.renwu = 1
                self._ack(f.seq, pr.ST_OK)
            elif cmd == pr.CMD_START_B:
                self.renwu = 2
                self._ack(f.seq, pr.ST_OK)
            elif cmd == pr.CMD_START_C:
                self.renwu = 3
                self._ack(f.seq, pr.ST_OK)
            elif cmd == pr.CMD_CLEAR_FAULT:
                self.drive_trips = 0
                self._ack(f.seq, pr.ST_OK)
            elif cmd == pr.CMD_SAVE_PARAMS:
                self.nvram = 1
                self.nvram_ms = self.now
                self._ack(f.seq, pr.ST_OK)
            elif cmd in (pr.CMD_RESET, pr.CMD_CALIB_GYRO, pr.CMD_SET_LOGGING):
                self._ack(f.seq, pr.ST_NOT_READY)
                self._text("[sim] 该命令仿真里没有实现（真车会给出真实答复）")
            else:
                self._ack(f.seq, pr.ST_BAD_CMD)
        else:
            self._ack(f.seq, pr.ST_UNKNOWN_TYPE)

    def _servo_apply(self, n: int):
        self.action = n
        self.servo_deg = [float(self.actions[n - 1][0]), float(self.actions[n - 1][1])]


def _param_crc() -> int:
    """与固件 host_param_table_crc() 同思路的一个稳定校验值（仿真用，不要求和真车一致）。"""
    h = 0xFFFF
    for p in P.PARAMS:
        for b in (p.id & 0xFF, p.id >> 8, int(p.default) & 0xFF, int(p.default * 100) & 0xFF):
            h = ((h << 5) + h + b) & 0xFFFF
    return h
