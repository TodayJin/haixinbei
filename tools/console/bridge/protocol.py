"""帧协议编解码 —— 与 `firmware/App/Svc/host_link.c` 一一对应（契约见 ../docs/API.md §4）。

帧格式：`A5 5A | type | seq | len | payload[len] | crc16_lo crc16_hi`
CRC 是 CRC16/CCITT-FALSE（poly 0x1021、init 0xFFFF、不反转），覆盖 `type..payload`。

这个模块**只依赖标准库**，双向都被测试覆盖（见 ../tests/test_protocol.py）；
固件里同一份逻辑在 host_link.c，任何一边改格式都要先改 docs/API.md。
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

# ---------------------------------------------------------------- 常量

SOF0 = 0xA5
SOF1 = 0x5A
MAX_PAYLOAD = 128

T_TELEM = 0x01
T_TELEM2 = 0x03
T_PARAM_GET = 0x10
T_PARAM_SET = 0x11
T_PARAM_REPORT = 0x12
T_PARAM_SAVE = 0x13
T_CAL_GET = 0x14
T_CAL_REPORT = 0x15
T_ACTIONS_GET = 0x16
T_ACTIONS = 0x17
T_ACTION_SET = 0x18
T_ACTION_SAVE = 0x19
T_SERVO_SET = 0x1A
T_SERVO_PLAY = 0x1B
T_DRIVE = 0x1C
T_DRIVE_REPORT = 0x1D
T_SERVO_DEV = 0x1E          # 舵机软件中位偏差 {ch u8, dev_us i16}（契约 /api/servo/deviation）
T_CMD = 0x20
T_ACK = 0x21
T_HELLO = 0x40
T_HELLO_REQ = 0x41
T_TEXT = 0x50

TYPE_NAMES = {
    T_TELEM: "telem", T_TELEM2: "telem2", T_PARAM_GET: "param_get",
    T_PARAM_SET: "param_set", T_PARAM_REPORT: "param_report", T_PARAM_SAVE: "param_save",
    T_CAL_GET: "cal_get", T_CAL_REPORT: "cal_report", T_ACTIONS_GET: "actions_get",
    T_ACTIONS: "actions", T_ACTION_SET: "action_set", T_ACTION_SAVE: "action_save",
    T_SERVO_SET: "servo_set", T_SERVO_PLAY: "servo_play", T_DRIVE: "drive",
    T_DRIVE_REPORT: "drive_report", T_SERVO_DEV: "servo_dev", T_CMD: "cmd", T_ACK: "ack", T_HELLO: "hello",
    T_HELLO_REQ: "hello_req", T_TEXT: "text",
}

ST_OK = 0x00
ST_UNKNOWN_TYPE = 0x02
ST_BAD_PARAM = 0x03
ST_OUT_OF_RANGE = 0x04
ST_NOT_READY = 0x05
ST_BAD_CMD = 0x06
ST_BUSY = 0x07

ST_NAMES = {
    ST_OK: "ok", ST_UNKNOWN_TYPE: "未知帧类型", ST_BAD_PARAM: "参数非法",
    ST_OUT_OF_RANGE: "超出范围", ST_NOT_READY: "当前不可用", ST_BAD_CMD: "命令非法",
    ST_BUSY: "忙",
}

CMD_STOP = 0x01
CMD_START_A = 0x02
CMD_START_B = 0x03
CMD_START_C = 0x04
CMD_CALIB_GYRO = 0x05
CMD_SAVE_PARAMS = 0x06
CMD_CLEAR_FAULT = 0x07
CMD_SET_TELEM_HZ = 0x08
CMD_RESET = 0x09
CMD_SET_LOGGING = 0x0A
CMD_SET_TELEM_MODE = 0x0F

CMD_BY_NAME = {
    "stop": CMD_STOP, "start_a": CMD_START_A, "start_b": CMD_START_B, "start_c": CMD_START_C,
    "calib_gyro": CMD_CALIB_GYRO, "save_params": CMD_SAVE_PARAMS, "clear_fault": CMD_CLEAR_FAULT,
    "telem_hz": CMD_SET_TELEM_HZ, "reset": CMD_RESET, "logging": CMD_SET_LOGGING,
    "telem_mode": CMD_SET_TELEM_MODE,
}

FL_GYRO_OK = 0x01
FL_LOST = 0x02
FL_HOST_DRIVE = 0x04
FL_SLOPE = 0x08
FL_MOTOR = 0x10
FL_LOGGING = 0x20
FL_FLASH = 0x40
FL_FAULT = 0x80

CAP_TELEM2 = 1 << 0
CAP_ACTIONS = 1 << 1
CAP_SERVO = 1 << 2
CAP_DRIVE = 1 << 3
CAP_CAL = 1 << 4
CAP_FLASH = 1 << 5
CAP_WATCHDOG = 1 << 6

TELEM_LEN = 32
TELEM2_LEN = 64

# nvram 状态（与固件的 NVRAM_STATE_* 对齐，见 API.md §4）
NVRAM_NAMES = {0: "空闲", 1: "待保存", 2: "保存中", 3: "已保存", 4: "失败"}
MODE_NAMES = {0: "空闲", 1: "陀螺", 2: "循迹", 3: "串级"}

ACTION_COUNT = 14
DRIVE_OFF, DRIVE_WHEELS, DRIVE_BASE_TURN, DRIVE_TRACE = 0, 1, 2, 3


# ---------------------------------------------------------------- CRC / 组帧

def crc16_ccitt(data: bytes) -> int:
    """CRC16/CCITT-FALSE（和固件 host_link.c 的 crc16_ccitt 一致）。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload 太长：%d > %d" % (len(payload), MAX_PAYLOAD))
    body = bytes([ftype & 0xFF, seq & 0xFF, len(payload)]) + bytes(payload)
    crc = crc16_ccitt(body)
    return bytes([SOF0, SOF1]) + body + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


@dataclass
class Frame:
    type: int
    seq: int
    payload: bytes = b""

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, "0x%02X" % self.type)


@dataclass
class FrameDecoder:
    """逐字节喂进来，吐出合法帧；同时统计 CRC 错/重同步次数。"""
    crc_errors: int = 0
    resyncs: int = 0
    _buf: bytearray = field(default_factory=bytearray)

    def reset(self) -> None:
        self._buf.clear()

    def feed(self, data: bytes) -> list[Frame]:
        out: list[Frame] = []
        self._buf.extend(data)
        while True:
            buf = self._buf
            # 找帧头
            if len(buf) < 2:
                break
            if buf[0] != SOF0 or buf[1] != SOF1:
                idx = buf.find(bytes([SOF0]))
                if idx < 0:
                    self.resyncs += 1
                    buf.clear()
                    break
                if idx > 0:
                    self.resyncs += 1
                    del buf[:idx]
                if len(buf) < 2:
                    break
                if buf[1] != SOF1:
                    # 形如 A5 A5 ...：只丢一个字节再试
                    self.resyncs += 1
                    del buf[0]
                    continue
            if len(buf) < 5:
                break
            plen = buf[4]
            if plen > MAX_PAYLOAD:
                # 长度离谱 = 这不是真帧头
                self.resyncs += 1
                del buf[0]
                continue
            total = 7 + plen
            if len(buf) < total:
                break
            body = bytes(buf[2:5 + plen])
            got = buf[5 + plen] | (buf[6 + plen] << 8)
            if crc16_ccitt(body) != got:
                self.crc_errors += 1
                del buf[0]
                continue
            out.append(Frame(buf[2], buf[3], bytes(buf[5:5 + plen])))
            del buf[:total]
        return out


# ---------------------------------------------------------------- 小端读写

def u16(p: bytes, off: int) -> int:
    return p[off] | (p[off + 1] << 8)


def i16(p: bytes, off: int) -> int:
    v = u16(p, off)
    return v - 0x10000 if v & 0x8000 else v


def u32(p: bytes, off: int) -> int:
    return p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (p[off + 3] << 24)


def f32(p: bytes, off: int) -> float:
    return struct.unpack_from("<f", p, off)[0]


def pack_u16(v: int) -> bytes:
    return struct.pack("<H", int(v) & 0xFFFF)


def pack_i16(v: int) -> bytes:
    v = int(v)
    v = max(-32768, min(32767, v))
    return struct.pack("<h", v)


def pack_f32(v: float) -> bytes:
    return struct.pack("<f", float(v))


# ---------------------------------------------------------------- 解帧

def decode_hello(p: bytes) -> dict:
    """T_HELLO 24 字节：fw u16 / proto u8 / param_crc u16 / telem_hz u16 / name[12] / 对齐 / caps u32"""
    if len(p) < 24:
        raise ValueError("HELLO 长度不对：%d" % len(p))
    name = p[8:20].split(b"\x00")[0].decode("utf-8", "replace")
    fw = u16(p, 0)
    return {
        "name": name,
        "version": "%d.%d.%d" % ((fw >> 8) & 0xFF, fw & 0xFF, 0),
        "fw_raw": fw,
        "proto": p[2],
        "param_crc": u16(p, 4),
        "telem_hz": u16(p, 6),
        "caps": u32(p, 20),
    }


def decode_telemetry(p: bytes) -> dict:
    """T_TELEM(32B) 与 T_TELEM2(64B) 共用：前 32 字节逐字节相同。

    字段布局以 firmware/App/Svc/host_link.c 的 put_telemetry_base/send_telemetry2 为准；
    `t_host` 由桥在收到帧时补上（固件不知道上位机的时钟）。
    """
    if len(p) < TELEM_LEN:
        raise ValueError("遥测长度不对：%d" % len(p))
    flags = p[30]
    lm = p[26]
    d = {
        "t": u32(p, 0),
        "yaw": i16(p, 4) / 10.0,
        "pitch": i16(p, 6) / 10.0,
        "roll": i16(p, 8) / 10.0,
        "vl": i16(p, 10),
        "vr": i16(p, 12),
        "pwm_l": i16(p, 14),
        "pwm_r": i16(p, 16),
        "vbat_mv": u16(p, 18),
        "ibat_ma": i16(p, 20),
        "servo1_us": u16(p, 22),
        "servo2_us": u16(p, 24),
        "line": [(lm >> i) & 1 for i in range(8)],
        "state_id": p[27],
        "loop_us": u16(p, 28),
        "flags": {
            "gyro_ok": bool(flags & FL_GYRO_OK),
            "lost": bool(flags & FL_LOST),
            "host_drive": bool(flags & FL_HOST_DRIVE),
            "slope": bool(flags & FL_SLOPE),
            "motor": bool(flags & FL_MOTOR),
            "logging": bool(flags & FL_LOGGING),
            "flash": bool(flags & FL_FLASH),
            "fault": bool(flags & FL_FAULT),
        },
    }
    # 扩展量：老 32B 帧给"真实的空缺值"（不是假装有）
    d["gray"] = list(p[32:40]) if len(p) >= 40 else [0] * 8
    d["trace"] = {"pos": i16(p, 40) / 100.0 if len(p) >= 42 else 0.0,
                  "pid": i16(p, 42) if len(p) >= 44 else 0}
    d["angle"] = {"err": i16(p, 44) / 10.0 if len(p) >= 46 else 0.0,
                  "rate_ref": i16(p, 46) / 10.0 if len(p) >= 48 else 0.0,
                  "corr": i16(p, 48) if len(p) >= 50 else 0}
    d["mode"] = p[50] if len(p) >= 51 else 0
    d["renwu"] = p[51] if len(p) >= 52 else 0
    d["servo1_deg"] = p[52] if len(p) >= 53 else 0
    d["servo2_deg"] = p[53] if len(p) >= 54 else 0
    d["action"] = p[54] if len(p) >= 55 else 0
    d["nvram"] = p[55] if len(p) >= 56 else 0
    d["drive"] = {"mode": p[56] if len(p) >= 57 else 0,
                  "enabled": bool(p[57]) if len(p) >= 58 else False}
    d["cross"] = bool(p[58]) if len(p) >= 59 else False
    d["watchdog_trips"] = p[59] if len(p) >= 60 else 0
    d["servo_dev"] = {"1": i16(p, 60) if len(p) >= 62 else 0,
                      "2": i16(p, 62) if len(p) >= 64 else 0}
    return d


def decode_param_report(p: bytes) -> tuple[int, float]:
    if len(p) < 6:
        raise ValueError("参数回报长度不对：%d" % len(p))
    return u16(p, 0), f32(p, 2)


def decode_actions(p: bytes) -> list[tuple[int, int]]:
    n = len(p) // 2
    return [(p[i * 2], p[i * 2 + 1]) for i in range(n)]


def decode_cal(p: bytes) -> dict:
    """trace_cal_data_t（82 字节，小端、字段间有对齐空洞）：
    valid u8@0 | pol u8[8]@1 | bad u8[8]@9 | pad@17 | blk u16[8]@18 | wht u16[8]@34
    | thr u16[8]@50 | jit u16[8]@66
    """
    if len(p) < 82:
        raise ValueError("标定回报长度不对：%d" % len(p))
    valid = p[0]
    ch = []
    for i in range(8):
        blk = u16(p, 18 + i * 2)
        wht = u16(p, 34 + i * 2)
        thr = u16(p, 50 + i * 2)
        jit = u16(p, 66 + i * 2)
        ok = bool(valid & (1 << i)) and blk > 0
        reason = 0 if ok else (1 if blk == 0 else 2)
        ch.append({"i": i, "blk": blk, "wht": wht, "thr": thr, "jitter": jit,
                   "pol": p[1 + i], "bad": p[9 + i], "ok": ok, "reason": reason,
                   "level": 0, "thr_level": 0})
    return {"active_set": 0, "quality": sum(1 for c in ch if c["ok"]), "ch": ch}


def decode_drive_report(p: bytes) -> dict:
    if len(p) < 12:
        raise ValueError("驾驶回报长度不对：%d" % len(p))
    return {"mode": p[0], "enabled": bool(p[1]), "a": i16(p, 2), "b": i16(p, 4),
            "pwm_l": i16(p, 6), "pwm_r": i16(p, 8),
            "servo1_deg": p[10], "servo2_deg": p[11]}


def decode_ack(p: bytes) -> dict:
    """T_ACK 载荷：`{seq u8, status u8}`。"""
    seq = p[0] if len(p) > 0 else 0
    st = p[1] if len(p) > 1 else 0
    return {"seq": seq, "status": st, "status_name": ST_NAMES.get(st, f"0x{st:02X}")}


def decode_text(p: bytes) -> str:
    return p.decode("utf-8", "replace").rstrip("\r\n")


# ---------------------------------------------------------------- 组帧助手

def frame_param_set(seq: int, pid: int, value: float) -> bytes:
    return build_frame(T_PARAM_SET, seq, pack_u16(pid) + pack_f32(value))


def frame_param_get(seq: int, pid: int = 0xFFFF) -> bytes:
    return build_frame(T_PARAM_GET, seq, pack_u16(pid))


def frame_param_save(seq: int) -> bytes:
    return build_frame(T_PARAM_SAVE, seq, b"")


def frame_cal_get(seq: int) -> bytes:
    return build_frame(T_CAL_GET, seq, b"")


def frame_actions_get(seq: int) -> bytes:
    return build_frame(T_ACTIONS_GET, seq, b"")


def frame_action_set(seq: int, n: int, a1: int, a2: int) -> bytes:
    return build_frame(T_ACTION_SET, seq, bytes([n & 0xFF, a1 & 0xFF, a2 & 0xFF]))


def frame_action_save(seq: int) -> bytes:
    return build_frame(T_ACTION_SAVE, seq, b"")


def frame_servo_set(seq: int, ch: int, deg: int) -> bytes:
    return build_frame(T_SERVO_SET, seq, bytes([ch & 0xFF]) + pack_i16(deg))


def frame_servo_play(seq: int, n: int) -> bytes:
    return build_frame(T_SERVO_PLAY, seq, bytes([n & 0xFF]))


def frame_drive(seq: int, mode: int, a: int = 0, b: int = 0) -> bytes:
    return build_frame(T_DRIVE, seq, bytes([mode & 0xFF]) + pack_i16(a) + pack_i16(b))


def frame_drive_report(seq: int) -> bytes:
    return build_frame(T_DRIVE_REPORT, seq, b"")


def frame_servo_dev(seq: int, ch: int, dev_us: int) -> bytes:
    return build_frame(T_SERVO_DEV, seq, bytes([ch & 0xFF]) + pack_i16(dev_us))


def frame_cmd(seq: int, cmd: int, arg: int = 0) -> bytes:
    return build_frame(T_CMD, seq, bytes([cmd & 0xFF, arg & 0xFF]))


def frame_hello_req(seq: int) -> bytes:
    return build_frame(T_HELLO_REQ, seq, b"")


def encode_telemetry2(t: dict) -> bytes:
    """`decode_telemetry` 的逆（仿真/回放用）——两边必须严格互逆，测试里有往返断言。"""
    p = bytearray(TELEM2_LEN)
    struct.pack_into("<I", p, 0, int(t.get("t", 0)) & 0xFFFFFFFF)
    struct.pack_into("<h", p, 4, int(round(t.get("yaw", 0.0) * 10)))
    struct.pack_into("<h", p, 6, int(round(t.get("pitch", 0.0) * 10)))
    struct.pack_into("<h", p, 8, int(round(t.get("roll", 0.0) * 10)))
    struct.pack_into("<h", p, 10, int(t.get("vl", 0)))
    struct.pack_into("<h", p, 12, int(t.get("vr", 0)))
    struct.pack_into("<h", p, 14, int(t.get("pwm_l", 0)))
    struct.pack_into("<h", p, 16, int(t.get("pwm_r", 0)))
    struct.pack_into("<H", p, 18, max(0, min(65535, int(t.get("vbat_mv", 0)))))
    struct.pack_into("<h", p, 20, max(-32768, min(32767, int(t.get("ibat_ma", 0)))))
    struct.pack_into("<H", p, 22, max(0, min(65535, int(t.get("servo1_us", 1500)))))
    struct.pack_into("<H", p, 24, max(0, min(65535, int(t.get("servo2_us", 1500)))))
    lm = 0
    for i, b in enumerate(t.get("line", [0] * 8)):
        if b:
            lm |= 1 << i
    p[26] = lm
    p[27] = int(t.get("state_id", 0)) & 0xFF
    struct.pack_into("<H", p, 28, max(0, min(65535, int(t.get("loop_us", 0)))))
    f = t.get("flags", {})
    p[30] = ((FL_GYRO_OK if f.get("gyro_ok") else 0) | (FL_LOST if f.get("lost") else 0)
             | (FL_HOST_DRIVE if f.get("host_drive") else 0)
             | (FL_SLOPE if f.get("slope") else 0) | (FL_MOTOR if f.get("motor") else 0)
             | (FL_LOGGING if f.get("logging") else 0) | (FL_FLASH if f.get("flash") else 0)
             | (FL_FAULT if f.get("fault") else 0))
    gray = list(t.get("gray", [0] * 8))[:8]
    gray += [0] * (8 - len(gray))
    p[32:40] = bytes(max(0, min(255, int(g))) for g in gray)
    struct.pack_into("<h", p, 40, int(round(t.get("trace", {}).get("pos", 0.0) * 100)))
    struct.pack_into("<h", p, 42, max(-32768, min(32767, int(t.get("trace", {}).get("pid", 0)))))
    struct.pack_into("<h", p, 44, int(round(t.get("angle", {}).get("err", 0.0) * 10)))
    struct.pack_into("<h", p, 46, int(round(t.get("angle", {}).get("rate_ref", 0.0) * 10)))
    struct.pack_into("<h", p, 48, max(-32768, min(32767, int(t.get("angle", {}).get("corr", 0)))))
    p[50] = int(t.get("mode", 0)) & 0xFF
    p[51] = int(t.get("renwu", 0)) & 0xFF
    p[52] = int(t.get("servo1_deg", 0)) & 0xFF
    p[53] = int(t.get("servo2_deg", 0)) & 0xFF
    p[54] = int(t.get("action", 0)) & 0xFF
    p[55] = int(t.get("nvram", 0)) & 0xFF
    p[56] = int(t.get("drive", {}).get("mode", 0)) & 0xFF
    p[57] = 1 if t.get("drive", {}).get("enabled") else 0
    p[58] = 1 if t.get("cross") else 0
    p[59] = int(t.get("watchdog_trips", 0)) & 0xFF
    dev = t.get("servo_dev", {})
    struct.pack_into("<h", p, 60, max(-32768, min(32767, int(dev.get("1", 0)))))
    struct.pack_into("<h", p, 62, max(-32768, min(32767, int(dev.get("2", 0)))))
    return bytes(p)


def telemetry_summary(t: dict) -> str:
    """给日志用的一行摘要。"""
    return ("yaw=%.1f pitch=%.1f roll=%.1f pwm=%d/%d vbat=%dmV ibat=%dmA "
            "mode=%s line=%s" % (
                t["yaw"], t["pitch"], t["roll"], t["pwm_l"], t["pwm_r"],
                t["vbat_mv"], t["ibat_ma"], MODE_NAMES.get(t["mode"], "?"),
                "".join(str(b) for b in t["line"])))
