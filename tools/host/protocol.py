"""上位机 <-> 主板 二进制通信协议 v1。

纯标准库实现，不依赖 pyserial，可以单独跑单元测试。

帧格式（全部小端 little-endian）：

    偏移  长度  字段     说明
    0     2     SOF      固定 A5 5A
    2     1     type     帧类型
    3     1     seq      序号；PC 下发的帧由 PC 分配，MCU 在 ACK 里原样回抄
    4     1     len      payload 长度，0..128
    5     len   payload  载荷
    5+len 2     crc16    CRC16/CCITT-FALSE，覆盖 type|seq|len|payload

设计要点
--------
1. 遥测帧定长（32 字节），上位机按类型直接定位，不做变长解析。
2. SOF + len + CRC 三重校验，任意单点噪声最多丢一帧，不会解出脏数据。
3. CRC 失败时只前移 1 字节重新找帧头（重同步），所以上电/插拔瞬间的
   乱码能自愈，不需要人工重连。
4. 参数名、单位、范围由上位机本地维护（帧里只传 id + 值），
   HELLO 帧里带 param_crc，两边表不一致时上位机会告警。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator

# ---------------------------------------------------------------- 常量

SOF = b"\xa5\x5a"
SOF0, SOF1 = 0xA5, 0x5A
PROTO_VERSION = 1
MAX_PAYLOAD = 128
FRAME_OVERHEAD = 7          # 2(SOF) + 3(type/seq/len) + 2(CRC)
HELLO_HEADER = 8            # fw(u16) + proto(u8) + rsv(u8) + param_crc(u16) + hz(u16)
HELLO_NAME = 12             # 板名，UTF-8，不足补 0          # 2(SOF) + 3(type/seq/len) + 2(CRC)

# 帧类型
T_TELEM        = 0x01   # MCU -> PC 遥测（定长 32）
T_LOG          = 0x30   # MCU -> PC 黑匣子记录（与遥测同构 + 附加字段）
T_PARAM_GET    = 0x10   # PC  -> MCU 读参数；payload = id(u16)，0xFFFF = 全部
T_PARAM_SET    = 0x11   # PC  -> MCU 写参数；payload = id(u16) + value(f32)
T_PARAM_REPORT = 0x12   # MCU -> PC 参数回读；payload = id(u16) + value(f32)
T_PARAM_SAVE   = 0x13   # PC  -> MCU 参数写 Flash
T_CMD          = 0x20   # PC  -> MCU 命令；payload = cmd(u8) + arg(i32)
T_ACK          = 0x21   # MCU -> PC 应答；payload = seq_req(u8) + status(u8)
T_HELLO_REQ    = 0x41   # PC  -> MCU 请求握手
T_HELLO        = 0x40   # MCU -> PC 握手
T_TEXT         = 0x50   # MCU -> PC 调试文本（printf 转发，UTF-8 或 GBK）

TYPE_NAMES = {
    T_TELEM: "TELEM", T_LOG: "LOG", T_PARAM_GET: "PARAM_GET",
    T_PARAM_SET: "PARAM_SET", T_PARAM_REPORT: "PARAM_REPORT",
    T_PARAM_SAVE: "PARAM_SAVE", T_CMD: "CMD", T_ACK: "ACK",
    T_HELLO_REQ: "HELLO_REQ", T_HELLO: "HELLO", T_TEXT: "TEXT",
}

# ACK 状态码
ST_OK           = 0x00
ST_BAD_CRC      = 0x01
ST_UNKNOWN_TYPE = 0x02
ST_BAD_PARAM    = 0x03
ST_OUT_OF_RANGE = 0x04
ST_NOT_READY    = 0x05
ST_BAD_CMD      = 0x06
STATUS_NAMES = {
    ST_OK: "OK", ST_BAD_CRC: "CRC 错", ST_UNKNOWN_TYPE: "未知帧类型",
    ST_BAD_PARAM: "参数 id 不存在", ST_OUT_OF_RANGE: "超范围",
    ST_NOT_READY: "未就绪", ST_BAD_CMD: "未知命令",
}

# 命令码
CMD_STOP          = 0x01
CMD_START_A       = 0x02
CMD_START_B       = 0x03
CMD_START_C       = 0x04
CMD_CALIB_GYRO    = 0x05
CMD_SAVE_PARAMS   = 0x06
CMD_CLEAR_FAULT   = 0x07
CMD_SET_TELEM_HZ  = 0x08
CMD_RESET         = 0x09
CMD_SET_LOGGING   = 0x0A
CMD_NAMES = {
    CMD_STOP: "停止", CMD_START_A: "启动任务A", CMD_START_B: "启动任务B",
    CMD_START_C: "启动任务C", CMD_CALIB_GYRO: "陀螺仪零偏校准",
    CMD_SAVE_PARAMS: "参数写Flash", CMD_CLEAR_FAULT: "清除故障",
    CMD_SET_TELEM_HZ: "设置遥测频率", CMD_RESET: "软复位",
    CMD_SET_LOGGING: "开始/停止记录",
}

# 遥测 flags 位定义
F_GYRO_OK   = 1 << 0
F_LINE_LOST = 1 << 1
F_STUCK     = 1 << 2
F_RAMP      = 1 << 3
F_FAULT     = 1 << 4
F_LOGGING   = 1 << 5
FLAG_NAMES = [
    (F_GYRO_OK, "陀螺正常"), (F_LINE_LOST, "循迹丢线"), (F_STUCK, "疑似卡死"),
    (F_RAMP, "坡道"), (F_FAULT, "故障"), (F_LOGGING, "记录中"),
]

# 8 路循迹线号 -> 位置权重（循迹模块从车左到车右）
LINE_WEIGHTS = [-3.5, -2.5, -1.5, -0.5, 0.5, 1.5, 2.5, 3.5]


# ---------------------------------------------------------------- CRC

def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC16/CCITT-FALSE：poly=0x1021, init=0xFFFF, 不反转, 不异或输出。

    与固件侧参考实现逐位一致（见 docs/上位机通信协议.md）。
    """
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------- 帧

@dataclass(slots=True)
class Frame:
    type: int
    payload: bytes = b""
    seq: int = 0

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, "0x%02X" % self.type)

    def to_bytes(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError("payload 过长: %d" % len(self.payload))
        body = struct.pack("<BBB", self.type, self.seq & 0xFF, len(self.payload)) + self.payload
        return SOF + body + struct.pack("<H", crc16_ccitt(body))

    def __repr__(self) -> str:
        return "Frame(%s seq=%d len=%d)" % (self.type_name, self.seq, len(self.payload))


@dataclass(slots=True)
class ParserStats:
    frames: int = 0
    crc_errors: int = 0
    length_errors: int = 0
    resyncs: int = 0
    bytes_in: int = 0


class FrameParser:
    """字节流 -> 帧。可增量喂数据（串口线程里用），也可以一次性解析。"""

    def __init__(self) -> None:
        self._buf = bytearray()
        self.stats = ParserStats()

    def feed(self, data: bytes) -> list[Frame]:
        if data:
            self._buf.extend(data)
            self.stats.bytes_in += len(data)
        out: list[Frame] = []
        while True:
            f = self._next()
            if f is None:
                break
            out.append(f)
            self.stats.frames += 1
        # 缓冲里再也凑不出帧时，最多留 1 字节（可能是半个 SOF）
        if len(self._buf) > 2 * MAX_PAYLOAD:
            self._buf = self._buf[-(MAX_PAYLOAD + 2):]
            self.stats.resyncs += 1
        return out

    def _next(self) -> Frame | None:
        buf = self._buf
        while True:
            # 找 SOF
            i = buf.find(SOF)
            if i < 0:
                # 保留最后一个可能是 SOF0 的字节
                if buf and buf[-1] == SOF0:
                    del buf[:-1]
                else:
                    buf.clear()
                return None
            if i > 0:
                del buf[:i]
                self.stats.resyncs += 1
            if len(buf) < 5:
                return None                      # 头部还没到齐
            ftype, seq, plen = buf[2], buf[3], buf[4]
            if plen > MAX_PAYLOAD:
                self.stats.length_errors += 1
                del buf[:2]                      # 丢掉这个假头，重新找
                continue
            total = 5 + plen + 2
            if len(buf) < total:
                return None                      # 载荷还没到齐
            body = bytes(buf[2:5 + plen])
            crc_rx = struct.unpack_from("<H", buf, 5 + plen)[0]
            if crc16_ccitt(body) != crc_rx:
                self.stats.crc_errors += 1
                del buf[:2]
                continue
            payload = bytes(buf[5:5 + plen])
            del buf[:total]
            return Frame(type=ftype, payload=payload, seq=seq)

    def reset(self) -> None:
        self._buf.clear()


def parse_all(data: bytes) -> list[Frame]:
    """一次性解析（测试/回放用）。"""
    return FrameParser().feed(data)


# ---------------------------------------------------------------- 遥测

# <IhhhhhhhHhHHBBHBB> 共 32 字节
TELEM_STRUCT = struct.Struct("<IhhhhhhhHhHHBBHBB")
TELEM_FIELDS = (
    "t_ms", "yaw_x10", "pitch_x10", "roll_x10", "v_l_mmps", "v_r_mmps",
    "pwm_l", "pwm_r", "vbat_mv", "ibat_ma", "servo1_us", "servo2_us",
    "line_mask", "state_id", "loop_us", "flags", "reserved",
)
assert TELEM_STRUCT.size == 32, TELEM_STRUCT.size


@dataclass(slots=True)
class Telemetry:
    """一帧遥测。角度单位 0.1 度，速度 mm/s，电压 mV，电流 mA，脉宽 us。"""

    t_ms: int = 0
    yaw_x10: int = 0
    pitch_x10: int = 0
    roll_x10: int = 0
    v_l_mmps: int = 0
    v_r_mmps: int = 0
    pwm_l: int = 0
    pwm_r: int = 0
    vbat_mv: int = 0
    ibat_ma: int = 0
    servo1_us: int = 0
    servo2_us: int = 0
    line_mask: int = 0
    state_id: int = 0
    loop_us: int = 0
    flags: int = 0
    reserved: int = 0

    # --- 派生量（上位机显示用，不进协议） ---
    @property
    def yaw(self) -> float:
        return self.yaw_x10 / 10.0

    @property
    def pitch(self) -> float:
        return self.pitch_x10 / 10.0

    @property
    def roll(self) -> float:
        return self.roll_x10 / 10.0

    @property
    def vbat(self) -> float:
        return self.vbat_mv / 1000.0

    @property
    def ibat(self) -> float:
        return self.ibat_ma / 1000.0

    @property
    def line_bits(self) -> list[int]:
        return [(self.line_mask >> i) & 1 for i in range(8)]

    @property
    def line_count(self) -> int:
        return bin(self.line_mask & 0xFF).count("1")

    @property
    def line_error(self) -> float:
        """加权位置误差，-3.5(最左) .. +3.5(最右)；丢线返回 0。"""
        bits = self.line_bits
        n = sum(bits)
        if n == 0:
            return 0.0
        return sum(w * b for w, b in zip(LINE_WEIGHTS, bits)) / n

    @property
    def flag_list(self) -> list[str]:
        return [name for bit, name in FLAG_NAMES if self.flags & bit]

    def to_bytes(self) -> bytes:
        return TELEM_STRUCT.pack(*[getattr(self, f) for f in TELEM_FIELDS])

    @classmethod
    def from_bytes(cls, payload: bytes) -> "Telemetry":
        if len(payload) != TELEM_STRUCT.size:
            raise ValueError("遥测帧长度应为 %d，实际 %d" % (TELEM_STRUCT.size, len(payload)))
        return cls(*TELEM_STRUCT.unpack(payload))


def build_telem_frame(t: Telemetry, seq: int = 0) -> bytes:
    return Frame(type=T_TELEM, payload=t.to_bytes(), seq=seq).to_bytes()


# ---------------------------------------------------------------- 参数表

@dataclass(slots=True)
class ParamSpec:
    pid: int                    # 参数 id（协议里传这个）
    key: str                    # 上位机内部键名 / 预设 JSON 里的键名
    name: str                   # 中文显示名
    unit: str
    default: float
    vmin: float
    vmax: float
    group: str
    note: str = ""              # 需要实测/存疑的，写在这里，界面会显示

    @property
    def id_hex(self) -> str:
        return "0x%04X" % self.pid


def _p(pid, key, name, unit, default, vmin, vmax, group, note="") -> ParamSpec:
    return ParamSpec(pid, key, name, unit, default, vmin, vmax, group, note)


# 默认值全部取自当前固件源码，不是编的：
#   循迹 PID  Kp/Ki/Kd = 2700/10.8/360，积分限幅 50   -> App/Svc/pid.c:15-17,29（旧板 375/1.5/50 ×7.2）
#   陀螺 PID  Kp/Ki/Kd = 150/0/15，积分限幅 1000   -> App/Svc/gyro_control.c 的 GYRO_*_FALLBACK（旧 100/1.5/10 ×7.2）
#     注意 Ki 默认 0：位置环积分会被静摩擦死区"喂"成常驻推力，车停在停车带外一直抽（实测 2.08~2.37° 不锁存）
#   基准速度 6480（循迹，旧 900 刻度）/ 2500（慢速，刻意不折算，1440 仍在死区以下）/ 6480（比赛任务流程）
#                                                   -> App/Svc/host_link.c SPECS
#   ×7.2 的理由：这两个环的增益输出的是「加在左右轮上的 PWM 偏置」，旧板满量程 1000、本板 7200（m02112）
#   PWM 上限 7200                                   -> App/Bsp/bsp_board.h:28
PARAMS: tuple[ParamSpec, ...] = (
    # ---- 循迹 PID ----
    _p(0x0201, "line_kp", "循迹 Kp", "", 2700.0, 0.0, 5000.0, "循迹PID"),
    _p(0x0202, "line_ki", "循迹 Ki", "", 10.8, 0.0, 200.0, "循迹PID"),
    _p(0x0203, "line_kd", "循迹 Kd", "", 360.0, 0.0, 1000.0, "循迹PID"),
    _p(0x0204, "line_ilimit", "循迹积分限幅", "", 50.0, 0.0, 10000.0, "循迹PID"),
    _p(0x0301, "base_speed", "循迹基准速度", "PWM", 6480.0, 0.0, 7200.0, "循迹PID"),
    _p(0x0302, "base_speed_slow", "慢速基准速度", "PWM", 2500.0, 0.0, 7200.0, "循迹PID"),
    _p(0x0304, "task_speed", "任务流程基准速度", "PWM", 6480.0, 0.0, 7200.0, "循迹PID",
       "比赛流程 renwu.c 里“旧板 900”对应的本板 PWM；旧板满量程 1000，本板 7200"),
    # ---- 速度环（目标转速 -> PWM；环在 App/Svc/speed_control.c）----
    _p(0x0601, "speed_kp", "速度 Kp", "", 20.0, 0.0, 2000.0, "速度环"),
    _p(0x0602, "speed_ki", "速度 Ki", "", 2.0, 0.0, 200.0, "速度环"),
    _p(0x0603, "speed_kd", "速度 Kd", "", 0.0, 0.0, 500.0, "速度环"),
    _p(0x0604, "speed_ilimit", "速度积分限幅", "", 3000.0, 0.0, 20000.0, "速度环"),
    _p(0x0605, "speed_ff_slope", "前馈斜率", "", 0.662, 0.0, 2.0, "速度环",
       "out = slope*target ± fric + PI；2026-09-30 台架实测拟合，换电机/电压要重标"),
    _p(0x0606, "speed_ff_fric", "前馈摩擦", "PWM", 2327.0, 0.0, 5000.0, "速度环",
       "摩擦等效压降；高速精度主要靠它，PI 只修残差"),
    # ---- 陀螺 / 转向 ----
    _p(0x0101, "gyro_kp", "陀螺 Kp", "", 150.0, 0.0, 2000.0, "陀螺转向",
       "2026-10-01 原地台架选定：旧刻度换算值 720 在本板自激，150 稳定"),
    _p(0x0102, "gyro_ki", "陀螺 Ki", "", 0.0, 0.0, 200.0, "陀螺转向"),
    _p(0x0103, "gyro_kd", "陀螺 Kd", "", 15.0, 0.0, 500.0, "陀螺转向",
       "≈Kp/10，压 20° 阶跃过冲到 4~10°"),
    _p(0x0104, "gyro_ilimit", "陀螺积分限幅", "", 1000.0, 0.0, 20000.0, "陀螺转向"),
    _p(0x0105, "turn_angle_offset", "固定转弯角", "度", 90.0, 1.0, 180.0, "陀螺转向"),
    _p(0x0401, "turn_tolerance", "转角容差", "度", 2.0, 0.1, 20.0, "陀螺转向",
       "改为按角度收尾后使用；现在是按时间收尾"),
    _p(0x0402, "turn_timeout", "转角超时", "ms", 1500.0, 100.0, 10000.0, "陀螺转向"),
    _p(0x0403, "yaw_rate_damp", "角速度阻尼", "", 1.0, 0.0, 20.0, "陀螺转向",
       "用 Gyro_Z 代替 yaw 差分做 D 项"),
    # ---- 事件 / 黑标 ----
    _p(0x0404, "marker_hold", "黑标确认次数", "次", 3.0, 1.0, 50.0, "事件检测",
       "连续 N 次整排压线才认一次事件"),
    _p(0x0405, "marker_min_gap", "事件最小间隔", "mm", 50.0, 0.0, 5000.0, "事件检测"),
    # ---- 车辆机械参数（都要实测） ----
    _p(0x0305, "counts_per_rev", "编码器计数/圈", "cnt", 1980.0, 1.0, 100000.0, "整车参数",
       "11PPR x4 x45 减速比"),
    _p(0x0306, "wheel_dia", "轮径", "mm", 65.0, 10.0, 300.0, "整车参数", "待实测"),
    _p(0x0307, "wheel_base", "轮距", "mm", 150.0, 10.0, 500.0, "整车参数", "待实测"),
    _p(0x0303, "pwm_max", "PWM 上限", "", 7200.0, 100.0, 7200.0, "整车参数"),
    # ---- 电源 ----
    _p(0x0501, "vbat_nominal", "标称电压", "mV", 12000.0, 3000.0, 30000.0, "电源保护"),
    _p(0x0502, "stall_current", "堵转判定电流", "mA", 2500.0, 100.0, 20000.0, "电源保护"),
    _p(0x0503, "vbat_low", "低压告警", "mV", 10500.0, 3000.0, 30000.0, "电源保护"),
)

PARAM_BY_ID = {p.pid: p for p in PARAMS}
PARAM_BY_KEY = {p.key: p for p in PARAMS}


def param_table_crc() -> int:
    """参数表指纹：固件与上位机各算一份，HELLO 时比对。"""
    data = b"".join(struct.pack("<Hf", p.pid, p.default) for p in PARAMS)
    return crc16_ccitt(data)


def group_names() -> list[str]:
    seen: list[str] = []
    for p in PARAMS:
        if p.group not in seen:
            seen.append(p.group)
    return seen


# ---------------------------------------------------------------- 组帧辅助

def build_param_get(pid: int = 0xFFFF, seq: int = 0) -> bytes:
    return Frame(T_PARAM_GET, struct.pack("<H", pid), seq).to_bytes()


def build_param_set(pid: int, value: float, seq: int = 0) -> bytes:
    return Frame(T_PARAM_SET, struct.pack("<Hf", pid, value), seq).to_bytes()


def build_param_save(seq: int = 0) -> bytes:
    return Frame(T_PARAM_SAVE, b"", seq).to_bytes()


def build_cmd(cmd: int, arg: int = 0, seq: int = 0) -> bytes:
    return Frame(T_CMD, struct.pack("<Bi", cmd, arg), seq).to_bytes()


def build_hello_req(seq: int = 0) -> bytes:
    return Frame(T_HELLO_REQ, b"", seq).to_bytes()


def parse_param_report(payload: bytes) -> tuple[int, float]:
    if len(payload) != 6:
        raise ValueError("参数回读帧长度应为 6，实际 %d" % len(payload))
    pid, value = struct.unpack("<Hf", payload)
    return pid, value


def parse_ack(payload: bytes) -> tuple[int, int]:
    if len(payload) != 2:
        raise ValueError("ACK 帧长度应为 2，实际 %d" % len(payload))
    return payload[0], payload[1]


@dataclass(slots=True)
class Hello:
    fw_version: int = 0
    proto_version: int = 0
    param_crc: int = 0
    telem_hz: int = 0
    name: str = ""

    @classmethod
    def from_bytes(cls, payload: bytes) -> "Hello":
        if len(payload) < HELLO_HEADER:
            raise ValueError("HELLO 帧太短: %d" % len(payload))
        fw, proto, _rsv, pcrc, hz = struct.unpack_from("<HBBHH", payload, 0)
        raw = payload[HELLO_HEADER:HELLO_HEADER + HELLO_NAME].split(b"\x00", 1)[0]
        try:
            name = raw.decode("utf-8")
        except UnicodeDecodeError:
            name = raw.decode("gbk", "replace")
        return cls(fw_version=fw, proto_version=proto, param_crc=pcrc,
                   telem_hz=hz, name=name)

    def to_bytes(self) -> bytes:
        nb = self.name.encode("utf-8")[:HELLO_NAME].ljust(HELLO_NAME, b"\x00")
        return struct.pack("<HBBHH", self.fw_version, self.proto_version,
                           0, self.param_crc, self.telem_hz) + nb


def decode_text(payload: bytes) -> str:
    for enc in ("utf-8", "gbk"):
        try:
            return payload.decode(enc)
        except UnicodeDecodeError:
            continue
    return payload.decode("utf-8", "replace")
