"""内置仿真信号源 —— `--sim` 模式（无车也能把整条链路跑起来，见 docs/API.md §6）。

仿真车实现了**同一套帧协议**（用 protocol.build_frame 组帧、再经 FrameDecoder 解回来，
所以它同时也在测协议层），行为上模仿真车：
  * 50Hz 遥测（yaw/pitch/roll、灰度 8 路、电池、舵机角度、模式/任务号/flags）；
  * 偶发丢线（`flags.lost`，灰度全白、重心冻结）；
  * 参数表 43 项可读可写（含范围校验，越界回 ST_OUT_OF_RANGE）；
  * 动作表 20 项、舵机两路、驾驶模式（wheels/base_turn/trace/off）+ 500ms 看门狗；
  * nvram 保存会走 待保存 → 保存中 → 已保存 的状态变化。
"""
from __future__ import annotations

import math
import random

from . import params as P
from . import protocol as pr

# 与 firmware/App/Bsp/servo.c 的 k_action_default 逐行一致（动作号从 1 开始）。
# 仿真车代表"刚上电、还没改过动作"的车（Servo_Init() 会调 Servo_ResetActionPoses()
# 把运行表复位成这张出厂表），所以初始时 20 个动作都不脏。
# 15..20 = 固件 servo.h 的 SERVO_ACTION_N=20 多出来的空位（角度由舵机页/任务编排下发）；
# 名字与角度必须和 bridge/params.py 的 ACTION_NAMES/ACTION_FACTORY 逐项一致 ——
# state.py 只在 len(acts) == P.ACTION_COUNT 时才认这份表，少一项就会被**静默丢弃**。
ACTION_NAMES = ["收回", "打下准备", "打下1", "打下2", "台阶2收回", "大坡1", "三级12",
                "三级2收回", "三级3打", "高台", "高台下", "收回2",
                "打下1(另一路)", "打下准备(另一路)",
                "动作15", "动作16", "动作17", "动作18", "动作19", "动作20"]
ACTION_DEFAULT = [(0, 0), (60, 120), (105, 120), (135, 150), (80, 170), (100, 120), (95, 120),
                  (40, 10), (100, 80), (115, 120), (85, 120), (40, 0), (90, 60), (30, 60),
                  (0, 0), (0, 0), (0, 0), (0, 0), (0, 0), (0, 0)]
COMPOUND = {5: "135/170 →0.4s→ 90 →0.1s→ 80", 9: "70/10 →0.2s→ 100/50 →0.1s→ 100/80"}

# ---- 角度环（MOTION_GYRO）仿真常数 ----------------------------------------
# 原来的仿真只把航向按左右轮差速**开环**积分，从没把 yaw 拉向 yaw_ref：驾驶模式
# base_turn 的目标航向（固件里 b = 角度 °）在仿真里根本看不见动作，朝向类指令
# 只能测到协议通不通。这里补一个和真车同结构的 P 环：误差 → 期望角速度 → 左右轮差速。
SIM_TICK_MS = 5.0            # 物理步长的标定基准（LinkRunner 默认 5ms 一拍）
ANGLE_KP_RATE = 1.5          # °/s per °（角度环 P）
ANGLE_RATE_MAX = 120.0       # 期望角速度限幅 °/s
ANGLE_RATE_DEAD = 0.6        # 死区：误差小于这个就不再差速，免得在原地抖
ANGLE_DV_PER_RATE = 13.4     # 左右轮差速 mm/s per (°/s)（由 yaw 积分常数反推）


def norm180(deg: float) -> float:
    """把角度收进 (-180, 180]：与固件 NormalizeAngle 同意图（角度环误差要走近路）。"""
    d = math.fmod(deg + 180.0, 360.0)
    if d <= 0.0:
        d += 360.0
    return d - 180.0


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
        self.yaw_base = -64.7                 # base 模式锁存的基准航向（= 固件 s_base_yaw）
        self.base_speed = 0.0                 # base_turn 的基准速度（角度环里做直行分量）
        self.last_phys_ms = 0
        self.pitch = 5.8
        self.roll = 4.7
        self.vbat = 12010
        self.ibat = 330
        self.loop_us = 412
        self.mode = 0
        # 遥测里的「解释器状态 / 当前任务」现在都从任务解释器镜像取（见 _telem_task_fields），
        # 不再有内置任务流程 A/B/C 的概念。
        self.lost = False
        self.lost_until = 0
        self.track = 3.6                      # 虚拟线上的位置（格）
        self.cal = None
        self._build_cal()

        # 任务程序（firmware/App/Svc/task_store.c 的仿真体：8 条记录 + 6144 B 程序区）
        self.tasks: dict[int, bytes] = {}           # 槽号 → 完整镜像（已 COMMIT）
        self.task_names: dict[int, str] = {}
        self.task_flags: dict[int, int] = {}        # 槽号 → 记录标志（保留位，原样往返）
        self.task_stage: dict[int, bytearray] = {}  # 槽号 → 4096 B 暂存（PUT 分片落这里）
        self.task_run = {"id": 0xFFFF, "state": 0xFF, "running": False, "block": 0,
                         "elapsed_ms": 0, "flags": 0}
        self.task_run_t0 = 0
        self.task_timeline: list[tuple[int, int, int]] = []   # 假任务的时间线（开跑时算，见 _prog_timeline）
        self.task_sig = None
        self.task_last_push = 0
        self.task_fx: list[str] = []      # 与 task_timeline 一一对应：这一步该做什么
        self.task_fx_step = None          # 上次应用过的步骤号（只在变化时应用）

    # ---------------------------------------------------------- 标定假数据
    def _build_cal(self):
        ch = []
        for i in range(8):
            blk = 3600 + i * 40
            wht = 900 + i * 15
            ch.append({"i": i, "blk": blk, "wht": wht, "thr": (blk + wht) // 2,
                       "jitter": 8 + i, "pol": 0, "bad": 0, "ok": True, "reason": 0,
                       "level": 0, "thr_level": 0})
                # ★2026-10-11：也发一套"另一套"（照固件 T_CAL_REPORT_B 的口径），
        #   否则传感器页的两套对比图在模拟器下永远只有一半，没法调前端。
        #   故意给一组**明显不同**的值（blk -35 / wht +45）—— 真车上"两套几乎一样"
        #   是常见坑（第二套在同一种材料上标的），对比图要能把它显示出来。
        other = [dict(c, blk=max(0, c["blk"] - 35), wht=c["wht"] + 45,
                     thr=(max(0, c["blk"] - 35) + c["wht"] + 45) // 2) for c in ch]
        self.cal = {"active_set": 0, "quality": 8, "ch": ch, "other": other}

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
        self._task_tick(now_ms)
        out: list[pr.Frame] = []
        if self.pending:
            out.extend(self.pending)
            self.pending = []
        if self.hz:
            period = max(1, int(1000 / self.hz))
            if self.next_tx + 2 * period < now_ms:
                # 落后太多（被别的东西卡住过）：重新对齐，不补一串出来
                self.next_tx = now_ms
            while now_ms >= self.next_tx:
                # 累加而不是"从当前时刻重算"：否则轮询步长（如 6ms）比周期大时，
                # 实际帧率会被拉到轮询频率上（要 100Hz 却只出 ~77Hz）；
                # 一拍里最多补 2 帧（上面刚重新对齐过），和真链路一次读回几帧一样
                self.next_tx += period
                out.append(pr.Frame(pr.T_TELEM2, 0, self._telem_bytes()))
        return out

    def _telem_task_fields(self) -> tuple[int, int]:
        """遥测里的 `(state_id, task_id)`：解释器当前状态号 / 当前任务号。

        和真固件同一口径：没载入/没在跑都是 `0xFF`（不是 0 —— 0 是合法的状态号/任务号）。
        """
        st = self.task_run
        if not st["running"]:
            return 0xFF, 0xFF
        tid = int(st["id"])
        return int(st["state"]) & 0xFF, (tid if 0 <= tid <= 0xFE else 0xFF)

    def _telem(self) -> dict:
        state_id, task_id = self._telem_task_fields()
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
            "state_id": state_id, "loop_us": self.loop_us,
            "mode": self.mode, "task_id": task_id, "action": self.action,
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

        # 这一拍有多长（按 SIM_TICK_MS 归一，标定常数都按 5ms 写）：链路 pump 换周期
        # 时航向/轮速的"每秒"行为不变。
        tick = 1.0 if not self.last_phys_ms else (now_ms - self.last_phys_ms) / SIM_TICK_MS
        tick = max(0.2, min(8.0, tick))
        self.last_phys_ms = now_ms

        # 角度环 + 速度环（MOTION_GYRO；驾驶模式 base_turn 走的就是这条）：
        # 航向误差 → 期望角速度（P 律 + 限幅 + 死区）→ 左右轮差速目标。
        # 少了这一段，仿真里 yaw 只是被左右轮开环积分，目标航向永远拉不回来。
        if self.mode == 1 and self.drive_enabled:
            err = norm180(self.yaw_ref - self.yaw)
            if abs(err) < ANGLE_RATE_DEAD:
                rate = 0.0
            else:
                rate = max(-ANGLE_RATE_MAX, min(ANGLE_RATE_MAX, err * ANGLE_KP_RATE))
            dv = rate * ANGLE_DV_PER_RATE
            self.vl_t = self.base_speed - dv
            self.vr_t = self.base_speed + dv

        # 转向 → 航向积分；左右轮差速 → 也带一点转向
        diff = (self.pwm_r - self.pwm_l) / 7200.0
        self.yaw += (diff * 3.0 + math.sin(now_ms / 900.0) * 0.05) * tick
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
        p[0] = 1                 # valid = 整套 0/1 标志（与固件一致，不是位掩码）
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
        p[0:2] = pr.pack_u16(0x0003)   # 跟固件 HOST_FW_VERSION 保持一致
        p[2] = 2
        p[4:6] = pr.pack_u16(_param_crc())
        p[6:8] = pr.pack_u16(self.hz)
        name = b"quandi-car"
        p[8:8 + len(name)] = name
        p[20:24] = bytes([0x7F | pr.CAP_TASK, 0, 0, 0])   # bit7 = HOST_CAP_TASK
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
            if self.task_run["running"]:
                self._ack(f.seq, pr.ST_NOT_READY)
                self._text("[sim] drive refused: task running")
            elif mode not in (0, 1, 2, 3):
                self._ack(f.seq, pr.ST_BAD_PARAM)
            else:
                self.drive_mode = mode
                self.drive_enabled = mode != 0
                if mode == 1:
                    self.vl_t, self.vr_t = float(a), float(b)
                    self.base_speed = 0.0
                    self.mode = 0
                elif mode == 2:
                    # 与固件 HostExt_DriveSet 同口径：**只在进入 base 模式时锁存基准航向**，
                    # 之后重复帧（桥每 200ms 的保活/新目标）只改目标偏移，不会把目标
                    # 拖着跟车一起跑 —— 拉不住的话角度环就永远追不上。
                    if self.mode != 1:
                        self.yaw_base = self.yaw
                    self.base_speed = float(a)
                    self.mode = 1
                    self.yaw_ref = self.yaw_base + float(b)
                elif mode == 3:
                    self.base_speed = 0.0
                    self.mode = 3
                else:
                    self.vl_t = self.vr_t = 0.0
                    self.base_speed = 0.0
                    self.mode = 0
                self._ack(f.seq, pr.ST_OK)
            self._drive_frame(f.seq)
        elif f.type == pr.T_DRIVE_REPORT:
            self._drive_frame(f.seq)
        elif f.type == pr.T_CMD:
            cmd = f.payload[0] if f.payload else 0
            arg = pr.decode_cmd_arg(f.payload)      # 与固件同口径：5 字节 = 4 字节小端
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
                self._task_stop_run()          # CMD_STOP 现在也能停住正在跑的任务
                self._ack(f.seq, pr.ST_OK)
            elif cmd in (pr.CMD_START_A, pr.CMD_START_B, pr.CMD_START_C):
                # 内置任务流程已从固件删除 ⇒ 这三个命令不再有任何作用（与真固件同口径）
                self._ack(f.seq, pr.ST_UNSUPPORTED)
                self._text("[sim] start_a/b/c 已废弃：任务现在走 T_TASK_RUN")
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
        elif f.type == pr.T_TASK_LIST:
            self._task_list(f.seq)
        elif f.type == pr.T_TASK_PUT:
            self._task_put(f.seq, f.payload)
        elif f.type == pr.T_TASK_COMMIT:
            self._task_commit(f.seq, f.payload)
        elif f.type == pr.T_TASK_GET:
            self._task_get(f.seq, f.payload)
        elif f.type == pr.T_TASK_DEL:
            self._task_del(f.seq, f.payload)
        elif f.type == pr.T_TASK_RUN:
            self._task_run(f.seq, f.payload)
        elif f.type == pr.T_TASK_STOP:
            self._task_stop(f.seq)
        elif f.type == pr.T_TASK_STATUS:
            self.pending.append(pr.Frame(pr.T_TASK_STATUS, f.seq, self._task_status_payload()))
        else:
            self._ack(f.seq, pr.ST_UNKNOWN_TYPE)

    # ---------------------------------------------------------- 任务程序（0x60..0x69）
    # 与固件口径对齐：TASK_STORE_SLOTS 8 条记录（页 58 目录）+ 页 59..61 共 6144 B 程序区，
    # 单条 ≤ TASK_STORE_CAP 4096 B（= pr.TASK_SLOT_BYTES，解释器 RAM 上限）。
    TASK_SLOTS = 8
    TASK_PROG_BYTES = 6144
    FAKE_BLOCK_MS = 300          # 假任务"每块走多久"（纯为让界面动起来，不参与任何真逻辑）
    FAKE_MAIN_MS = 800           # 假任务"每次回主页停多久"（主页是靠分派瞬间跳走的，给一点停留才看得见）

    def _task_status_payload(self) -> bytes:
        return pr.payload_task_status(self.task_run)

    def _task_crc(self, buf: bytes) -> int:
        return pr.crc16_ccitt(buf[12:])

    def _task_list(self, seq: int):
        """一条任务一帧（空槽不发），最后补一帧空载荷收尾 —— 见 docs/任务编程.md §5。"""
        for tid in sorted(self.tasks):
            buf = self.tasks[tid]
            self.pending.append(pr.Frame(pr.T_TASK_LIST_R, seq,
                                         pr.encode_task_list_r(tid, len(buf), self._task_crc(buf),
                                                               self.task_flags.get(tid, 0),
                                                               self.task_names.get(tid, ""))))
        self.pending.append(pr.Frame(pr.T_TASK_LIST_R, seq, b""))

    def _task_put(self, seq: int, p: bytes):
        if len(p) < 4:
            self._ack(seq, pr.ST_BAD_PARAM)
            return
        tid, off, data = pr.u16(p, 0), pr.u16(p, 2), p[4:]
        if tid >= self.TASK_SLOTS or off + len(data) > pr.TASK_SLOT_BYTES:
            self._ack(seq, pr.ST_OUT_OF_RANGE)
            return
        buf = self.task_stage.setdefault(tid, bytearray([0xFF] * pr.TASK_SLOT_BYTES))
        buf[off:off + len(data)] = data          # 分片可乱序、可重叠（和固件暂存一样）
        self._ack(seq, pr.ST_OK)

    def _task_commit(self, seq: int, p: bytes):
        if len(p) < 6:
            self._ack(seq, pr.ST_BAD_PARAM)
            return
        tid, length, crc = pr.u16(p, 0), pr.u16(p, 2), pr.u16(p, 4)
        flags = p[6] if len(p) > 6 else 0
        name = ""
        if len(p) > 7:
            n = p[7]
            if n > 30 or 8 + n > len(p):
                self._ack(seq, pr.ST_BAD_PARAM)   # 名字长度撒谎
                return
            name = p[8:8 + n].decode("utf-8", "replace")
        if self.task_run["running"]:
            # 真固件同一条铁律（host_link.c 的 T_TASK_COMMIT）：解释器在跑时擦一页会把
            # CPU 按住几十毫秒，正在跑的控制环直接失控 ⇒ 回 ST_BUSY，先 0x68 STOP。
            self._ack(seq, pr.ST_BUSY)
            return
        buf = self.task_stage.get(tid)
        if buf is None:
            self._ack(seq, pr.ST_NOT_READY)
            return
        if length < 12 or length > pr.TASK_SLOT_BYTES:
            self._ack(seq, pr.ST_OUT_OF_RANGE)
            return
        img = bytes(buf[:length])
        if self._task_crc(img) != crc:
            self._ack(seq, pr.ST_BAD_PARAM)      # 拒"烧了一半"的程序
            return
        # n_states 上限与固件一致：2026-10-09 起是 32（旧全程流程改成"主页分派 + 一段一状态"
        # 需要 22 个状态）。别只在这里写死一个更松的数 —— 仿真车放行的、真车必须也认。
        if img[0] != 0x54 or img[1] != 0x4B or img[2] != 1 or not (1 <= img[3] <= 32):
            self._ack(seq, pr.ST_BAD_PARAM)
            return
        used = sum(len(b) for k, b in self.tasks.items() if k != tid)
        if used + length > self.TASK_PROG_BYTES:
            self._ack(seq, pr.ST_BAD_PARAM)      # 程序区（页 59..61 = 6144 B）装不下
            return
        self.tasks[tid] = img
        if name:
            self.task_names[tid] = name
        # flags 是保留位：原样存（车上已经没人读它了）。bit0 曾经是"开机自启"，
        # 用户 m24999 把那个功能整个删了，模拟器也不再只认 bit0。
        if flags:
            self.task_flags[tid] = flags & 0xFF
        else:
            self.task_flags.pop(tid, None)
        self._ack(seq, pr.ST_OK)

    def _task_get(self, seq: int, p: bytes):
        tid = pr.u16(p, 0) if len(p) >= 2 else 0xFFFF
        buf = self.tasks.get(tid)
        if buf is None:
            self._ack(seq, pr.ST_NOT_READY)
            return
        off = 0
        while off < len(buf):
            chunk = buf[off:off + pr.TASK_DATA_CHUNK]
            self.pending.append(pr.Frame(pr.T_TASK_DATA, seq, pr.encode_task_data(tid, off, chunk)))
            off += len(chunk)
        self.pending.append(pr.Frame(pr.T_TASK_DATA, seq, pr.encode_task_data(tid, off, b"")))

    def _task_del(self, seq: int, p: bytes):
        tid = pr.u16(p, 0) if len(p) >= 2 else 0xFFFF
        if self.task_run["running"]:
            # 与固件一致：跑着的时候**不删也不停**，只回 ST_BUSY（先 0x68 STOP 再删）。
            # "悄悄帮你停掉任务再删"比拒绝更坏：操作员以为删的是个死任务。
            self._ack(seq, pr.ST_BUSY)
            return
        self.tasks.pop(tid, None)
        self.task_stage.pop(tid, None)
        self.task_names.pop(tid, None)
        self.task_flags.pop(tid, None)
        if self.task_run["id"] == tid:
            self._task_stop_run()
        self._ack(seq, pr.ST_OK)

    def _task_run(self, seq: int, p: bytes):
        tid = pr.u16(p, 0) if len(p) >= 2 else 0xFFFF
        if tid not in self.tasks:
            self._ack(seq, pr.ST_NOT_READY)
            return
        if self.task_run["running"]:
            self._ack(seq, pr.ST_BUSY)     # 已经有一个任务占着电机（固件同规则）
            return
        self.task_run = {"id": tid, "state": 1, "running": True, "block": 0,
                         "elapsed_ms": 0, "flags": 0}
        self.task_run_t0 = self.now
        # ★开跑时把"假时间线"算好（按真实执行流排，见 _prog_timeline）
        self.task_timeline = self._prog_timeline(self.tasks.get(tid, b""))
        self._ack(seq, pr.ST_OK)

    def _task_stop_run(self):
        """把解释器镜像置成"没在跑"（`state_id`/`task_id` 随之回 0xFF）。"""
        self.task_run.update({"state": 0xFF, "running": False, "block": 0, "elapsed_ms": 0})

    def _task_stop(self, seq: int):
        self._task_stop_run()
        self._ack(seq, pr.ST_OK)

    def _prog_shape(self, buf: bytes) -> list[tuple[int, int]]:
        """从任务镜像里读出"每个状态各有多少块" —— 照固件口径（task_prog.h）：

        `TASK_HDR_LEN=12`、`TASK_STATE_LEN=8`，每状态 8 字节 =
        `name_off u16 | enter_off u16 | body_off u16 | n_enter u8 | n_body u8`。
        """
        if len(buf) < 12 or pr.u16(buf, 0) != 0x4B54:
            return []
        n = buf[3]
        if n == 0 or len(buf) < 12 + n * 8:
            return []
        return [(buf[12 + i * 8 + 6], buf[12 + i * 8 + 7]) for i in range(n)]

    def _gate_targets(self, buf: bytes) -> list[int]:
        """主页循环体里那一串「如果…就跳」(`TASK_OP_IF_STAGE = 0x0D`) 各自跳到哪个状态，按出现顺序。

        块头 10 字节：`op u8 | flags u8 | p0 u16 | p1 i16 | p2 u16 | p3 u16` ⇒ **p2 在 +6**。
        """
        if len(buf) < 12 or pr.u16(buf, 0) != 0x4B54:
            return []
        n, main = buf[3], buf[4]
        if n == 0 or main >= n or len(buf) < 12 + n * 8:
            return []
        base = 12 + main * 8
        boff, nb = pr.u16(buf, base + 4), buf[base + 7]
        out: list[int] = []
        for i in range(nb):
            o = boff + i * 10
            if o + 10 > len(buf):
                break
            if buf[o] == 0x0D:                       # TASK_OP_IF_STAGE
                tgt = pr.u16(buf, o + 6)             # p2 = 目标状态
                if tgt < n:
                    out.append(tgt)
        return out

    def _block_fx(self, buf: bytes, si: int, ne: int, k: int) -> str:
        """第 si 状态第 k 块对**轮子/舵机**的效果（`k < ne` ⇒ 在 enter 序列，否则 body）。

        ★2026-10-11 加。以前这个模拟器的"假任务"**只走时间线、根本不跑块**（见 _task_tick 的
        说明）⇒ 任务跑着时 `vl_t/vr_t` 恒为 0 ⇒ **航位推算的路线图在模拟器里永远是空的**，
        没法验证「只在跑任务时才画」那套东西。现在真的按块改轮速目标。

        只实现**影响运动/舵机**的那几块（SET_SPEED / SET_MODE / END / 动作 / 舵机），
        其余块照旧"只占时间" —— 够让模拟器里的任务动起来、够验轨迹了。
        """
        n = buf[3]
        if si >= n or len(buf) < 12 + n * 8:
            return ""
        base = 12 + si * 8
        off = pr.u16(buf, base + (2 if k < ne else 4))
        idx = k if k < ne else k - ne
        if off == 0:
            return ""
        o = off + idx * 10
        if o + 10 > len(buf):
            return ""
        op = buf[o]
        p0 = pr.u16(buf, o + 2)
        if op == 0x01:                      # SET_SPEED：p0 = 目标速度（mm/s，两轮同速）
            return "spd:%d" % p0
        if op == 0x02:                      # SET_MODE：0 停 / 1 前进 / 2 后退 / 3 左转 / 4 右转
            return "mode:%d" % p0
        if op == 0x04:                      # ACTION：摆某个舵机动作
            return "act:%d" % p0
        if op == 0x09:                      # SET_SERVO：p0 = 通道，p1 = 角度
            return "servo:%d:%d" % (p0, int(pr.i16(buf, o + 4)))
        if op == 0x08:                      # END：停车
            return "stop"
        return ""

    def _prog_timeline(self, buf: bytes) -> list[tuple[int, int, int]]:
        """把任务摊成假任务要依次经过的 `[(状态号, 该序列内第几块, 停多久ms), ...]`。

        ★★**按真实执行流排，不是按镜像里块的物理顺序排**（用户指出的错）：
            主页循环体那是一串**分派**判断 —— 固件在**一拍之内**把它们全走完，走到第 k 条成立
            就 `seq_begin(目标)` **立刻跳走**，那些判断块**一点时间都不占**。
            真正的顺序是：
                主页(停 FAKE_MAIN_MS) → 状态A 的 enter 逐块 → 状态A 的 body 逐块
                → 主页(停 FAKE_MAIN_MS) → 状态B … → 回到第一条，循环
            所以这里**不把主页的块铺进时间线**，主页只作为"每次跳转之间的一小段停留"出现。
        （以前是按物理顺序铺的 ⇒ 看起来像"主页 14 块全走完才轮到第一个状态"，那是假的。）
        """
        shape = self._prog_shape(buf)
        if not shape:
            return []
        main = buf[4]
        tl: list[tuple[int, int, int]] = []
        fx: list[str] = []
        for tgt in self._gate_targets(buf):
            ne, nb = shape[tgt]
            for k in range(ne):
                tl.append((tgt, k, self.FAKE_BLOCK_MS))
                fx.append(self._block_fx(buf, tgt, ne, k))
            for k in range(nb):
                tl.append((tgt, k, self.FAKE_BLOCK_MS))
                fx.append(self._block_fx(buf, tgt, ne, ne + k))
            tl.append((main, 0, self.FAKE_MAIN_MS))     # 这一段做完，跳回主页再判下一条
            fx.append("")
        if not tl:
            # 没有分派门（**不是**"主页分派"写法）⇒ 退回按状态表的物理顺序铺。
            # 这种任务本来就是"一个状态跑完接下一个"的顺序结构，所以这就是它的真实流。
            # ★必须有这条兜底：否则块号永远不动 ⇒ _task_tick 的 sig 不变 ⇒ 一帧状态都不推
            #   （test_task.py 那条 RUN→STATUS 的测试就是靠"内容变了推一帧"拿到状态的）。
            for si, (ne, nb) in enumerate(shape):
                for k in range(ne):
                    tl.append((si, k, self.FAKE_BLOCK_MS))
                    fx.append(self._block_fx(buf, si, ne, k))
                for k in range(nb):
                    tl.append((si, k, self.FAKE_BLOCK_MS))
                    fx.append(self._block_fx(buf, si, ne, ne + k))
        self.task_fx = fx
        return tl

    def _task_tick(self, now_ms: int):
        """假任务往前走：状态/块号一变就推一帧，没变时 1Hz 心跳（和固件同口径）。

        ★2026-10-10 修：以前是 `block = 已跑毫秒 // 400`、`state` 写死 1 ——
        块号会涨到任务根本没有的地方（在"上台阶只有 18 块"的任务上实测报到 **第 77 块**），
        拿它调界面时看到的是假数字。
        现在**照这份任务镜像的真实状态表逐块走**（每块 `FAKE_BLOCK_MS` 毫秒，走完循环）：
          · 块号永远落在该状态的真实块数之内（enter/body 分开算，和固件的 `s_idx` 同口径）；
          · 状态号在 0..n-1 之内，并且会真的换状态 ⇒ 界面上的「监控跟随」也能在模拟器里试。
        它**仍然是假的**（没有真解释器，不跑块），只是不再报出结构上不可能的数字。
        """
        st = self.task_run
        if st["running"]:
            st["elapsed_ms"] = max(0, int(now_ms - self.task_run_t0))
            tl = getattr(self, "task_timeline", None) or []
            total = sum(d for _, _, d in tl)
            if total:
                t = st["elapsed_ms"] % total
                step = 0
                for step, (si, blk, dur) in enumerate(tl):
                    if t < dur:
                        st["state"], st["block"] = si, blk
                        break
                    t -= dur
                # ★2026-10-11：把那一步的效果真的应用上去（以前只改状态号、轮子恒 0
                #   ⇒ 任务跑着车不动 ⇒ 路线图在模拟器里是死的）。只在**步骤变化时**应用。
                if step != self.task_fx_step:
                    self.task_fx_step = step
                    if step < len(self.task_fx):
                        self._apply_fx(self.task_fx[step])
        sig = (st["id"], st["state"], 1 if st["running"] else 0, st["block"])
        if sig != self.task_sig or now_ms - self.task_last_push >= 1000:
            self.task_sig = sig
            self.task_last_push = now_ms
            self.pending.append(pr.Frame(pr.T_TASK_STATUS, 0, self._task_status_payload()))

    def _apply_fx(self, fx: str) -> None:
        """把"这一步该做什么"落到轮速/舵机上（`_block_fx` 产出的字符串）。"""
        if not fx:
            return
        kind, _, arg = fx.partition(":")
        try:
            if kind == "spd":
                self.vl_t = self.vr_t = float(arg)
            elif kind == "stop":
                self.vl_t = self.vr_t = 0.0
            elif kind == "mode":
                v = 300.0
                self.vl_t, self.vr_t = {
                    0: (0.0, 0.0), 1: (v, v), 2: (-v, -v),
                    3: (-v, v), 4: (v, -v),
                }.get(int(arg), (0.0, 0.0))
            elif kind == "act":
                n = int(arg)
                if 1 <= n <= len(self.actions):
                    self._servo_apply(n)
            elif kind == "servo":
                ch, _, deg = arg.partition(":")
                c = int(ch)
                if 1 <= c <= 2:
                    self.servo_deg[c - 1] = float(int(deg))
        except (TypeError, ValueError):
            pass

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
