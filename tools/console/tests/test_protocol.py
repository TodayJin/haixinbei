"""协议层：CRC 标准校验值、组帧/解帧往返、坏字节重同步、遥测布局逐字段核对。

遥测布局的断言**不用 encode_telemetry2()**（那是被测代码），而是用 struct.pack 按
firmware/App/Svc/host_link.c 的偏移手写一遍 —— 这样布局写错才会被抓到。
帧格式（同样以固件为准）：`A5 5A | type | seq(1B) | len(1B) | payload[len] | crc16_le`
（CRC16/CCITT-FALSE 覆盖 type..payload）。
"""
from __future__ import annotations

import pytest

import struct

from bridge import protocol as pr




def test_decode_cal_other_carries_active_and_second_set():
    """★2026-10-11：T_CAL_REPORT_B = [当前套号 u8][另一套 trace_cal_data_t]。

    固件一帧装不下两套（82×2=164 > MAX_PAYLOAD 128）所以分成两帧；这一帧既补"另一套"，
    也第一次把"到底哪套是当前"带回来（T_CAL_REPORT 里没有这个字节）。
    传感器页要同时画两套，全靠它。
    """
    body = bytearray(82)
    body[0] = 1          # valid
    body[18] = 200       # blk[0] 低字节
    body[34] = 44        # wht[0]
    active, other = pr.decode_cal_other(bytes([1]) + bytes(body))
    assert active == 1
    assert other["ch"][0]["blk"] == 200 and other["ch"][0]["wht"] == 44
    assert other["ch"][0]["ok"] is True

    # 长度不够要明确报错，不能静默给半套数据
    with pytest.raises(ValueError):
        pr.decode_cal_other(bytes([0]) + bytes(80))
def test_crc_matches_ccitt_false_check_value():
    # CRC-16/CCITT-FALSE 的标准校验值（poly 0x1021、init 0xFFFF、不反转）
    assert pr.crc16_ccitt(b"123456789") == 0x29B1
    assert pr.crc16_ccitt(b"") == 0xFFFF


def test_frame_roundtrip_and_header():
    payload = bytes([1, 2, 3, 4])
    raw = pr.build_frame(pr.T_PARAM_SET, 0x34, payload)
    assert raw[0] == 0xA5 and raw[1] == 0x5A
    assert raw[2] == pr.T_PARAM_SET
    assert raw[3] == 0x34                                     # seq 只 1 字节
    assert raw[4] == len(payload)
    assert raw[5:9] == payload
    crc = struct.unpack_from("<H", raw, 9)[0]
    assert crc == pr.crc16_ccitt(raw[2:9])
    assert len(raw) == 7 + len(payload)
    frames = pr.FrameDecoder().feed(raw)
    assert len(frames) == 1
    assert frames[0].type == pr.T_PARAM_SET and frames[0].seq == 0x34
    assert frames[0].payload == payload


def test_decoder_resyncs_on_garbage():
    d = pr.FrameDecoder()
    good = pr.build_frame(pr.T_TELEM2, 7, bytes(pr.TELEM2_LEN))
    junk = b"\x00\xff\xa5\x00\x11\x99"                      # 半截帧头 + 噪声
    frames = d.feed(junk + good + junk[:2])
    assert [f.type for f in frames] == [pr.T_TELEM2]
    assert d.resyncs >= 1
    # 坏 CRC 的帧要被丢掉，不能假装成功
    bad = bytearray(pr.build_frame(pr.T_TEXT, 1, b"hi"))
    bad[7] ^= 0xFF
    assert d.feed(bytes(bad)) == []
    assert d.crc_errors >= 1


def test_frame_splits_across_feeds_byte_by_byte():
    raw = pr.build_frame(pr.T_ACK, 9, bytes([9, 0]))
    d = pr.FrameDecoder()
    got = []
    for b in raw[:-1]:
        got += d.feed(bytes([b]))
    assert got == []
    got += d.feed(raw[-1:])
    assert len(got) == 1 and got[0].type == pr.T_ACK


def test_decoder_rejects_absurd_length():
    d = pr.FrameDecoder()
    # 帧头 + 长度字段 0xFE（> MAX_PAYLOAD）应被当成假帧头丢掉
    assert d.feed(bytes([0xA5, 0x5A, pr.T_TELEM, 0, 0xFE, 0, 0])) == []
    assert d.resyncs >= 1


def test_telemetry_layout_matches_firmware_offsets():
    p = bytearray(pr.TELEM2_LEN)
    struct.pack_into("<I", p, 0, 12345)          # t（上车毫秒）
    struct.pack_into("<h", p, 4, -647)           # yaw ×10
    struct.pack_into("<h", p, 6, 58)             # pitch ×10
    struct.pack_into("<h", p, 8, 47)             # roll ×10
    struct.pack_into("<h", p, 10, 0)             # vl（编码器未做，恒 0）
    struct.pack_into("<h", p, 12, 0)
    struct.pack_into("<h", p, 14, 1500)          # pwm_l
    struct.pack_into("<h", p, 16, -1500)
    struct.pack_into("<H", p, 18, 12010)         # vbat_mv
    struct.pack_into("<h", p, 20, 330)           # ibat_ma
    struct.pack_into("<H", p, 22, 1500)          # 舵机1 µs
    struct.pack_into("<H", p, 24, 1000)          # 舵机2 µs
    p[26] = 0b00011100                            # 线位掩码：第 2/3/4 路
    p[27] = 23                                    # black_num
    struct.pack_into("<H", p, 28, 412)            # loop_us
    p[30] = pr.FL_GYRO_OK | pr.FL_SLOPE | pr.FL_FLASH
    p[32:40] = bytes([5, 20, 35, 50, 65, 80, 95, 110])
    struct.pack_into("<h", p, 40, 425)            # pos ×100
    struct.pack_into("<h", p, 42, -123)           # pid
    struct.pack_into("<h", p, 44, 13)             # aerr ×10
    struct.pack_into("<h", p, 46, 25)             # arate ×10
    struct.pack_into("<h", p, 48, -3)             # acorr
    p[50] = 3                                     # mode=串级
    p[51] = 1                                     # task_id=1（当前任务号）
    p[52] = 90
    p[53] = 45
    p[54] = 6                                     # 动作号
    p[55] = 2                                     # nvram 保存中
    p[56] = 1                                     # drive=wheels
    p[57] = 1
    p[58] = 1                                     # cross
    p[59] = 4                                     # watchdog trips
    struct.pack_into("<h", p, 60, 12)             # 舵机1 偏差 µs
    struct.pack_into("<h", p, 62, -8)             # 舵机2 偏差 µs

    t = pr.decode_telemetry(bytes(p))
    assert t["t"] == 12345
    assert t["yaw"] == -64.7 and t["pitch"] == 5.8 and t["roll"] == 4.7
    assert t["vl"] == 0 and t["vr"] == 0
    assert t["pwm_l"] == 1500 and t["pwm_r"] == -1500
    assert t["vbat_mv"] == 12010 and t["ibat_ma"] == 330
    assert t["servo1_us"] == 1500 and t["servo2_us"] == 1000
    assert t["line"] == [0, 0, 1, 1, 1, 0, 0, 0]
    assert t["state_id"] == 23 and t["loop_us"] == 412
    assert t["flags"] == {"gyro_ok": True, "lost": False, "host_drive": False,
                          "slope": True, "motor": False, "logging": False,
                          "flash": True, "fault": False}
    assert t["gray"] == [5, 20, 35, 50, 65, 80, 95, 110]
    assert t["trace"] == {"pos": 4.25, "pid": -123}
    assert t["angle"] == {"err": 1.3, "rate_ref": 2.5, "corr": -3}
    assert t["mode"] == 3 and t["task_id"] == 1
    assert t["servo1_deg"] == 90 and t["servo2_deg"] == 45
    assert t["action"] == 6 and t["nvram"] == 2
    assert t["drive"] == {"mode": 1, "enabled": True}
    assert t["cross"] is True and t["watchdog_trips"] == 4
    assert t["servo_dev"] == {"1": 12, "2": -8}


def test_telemetry_short_frame_leaves_extended_fields_empty():
    # 老 32B 帧：扩展量必须是"真实空缺值"，不能拿垃圾字节冒充
    p = bytearray(pr.TELEM_LEN)
    struct.pack_into("<I", p, 0, 77)
    struct.pack_into("<h", p, 4, 100)
    p[26] = 0b00000001
    t = pr.decode_telemetry(bytes(p))
    assert t["yaw"] == 10.0 and t["line"][0] == 1
    assert t["gray"] == [0] * 8
    assert t["trace"] == {"pos": 0.0, "pid": 0}
    assert t["angle"] == {"err": 0.0, "rate_ref": 0.0, "corr": 0}
    assert t["mode"] == 0 and t["servo_dev"] == {"1": 0, "2": 0}


def test_telemetry_task_id_is_ff_when_absent_or_idle():
    # 新语义：`task_id`（原 renwu）0xFF = 没在跑。
    # ①老 32B 帧根本没这个字节 ⇒ 必须回 0xFF，不能假装是任务 0
    p = bytearray(pr.TELEM_LEN)
    struct.pack_into("<I", p, 0, 77)
    t = pr.decode_telemetry(bytes(p[:32]))
    assert t["task_id"] == 0xFF, "短帧缺 task_id 时必须回「没在跑」，不能是 0"
    # ②完整帧里车真的报了 0xFF ⇒ 原样透传（不要被归一成 0）
    p = bytearray(pr.TELEM2_LEN)
    struct.pack_into("<I", p, 0, 78)
    p[27] = 0xFF                                  # 解释器状态号：没载入/没在跑
    p[51] = 0xFF                                  # 当前任务号：没在跑
    t = pr.decode_telemetry(bytes(p))
    assert t["task_id"] == 0xFF and t["state_id"] == 0xFF
    # ③编回去也得是 0xFF（encode/decode 对这两个字段互逆）
    assert pr.encode_telemetry2(t)[27] == 0xFF and pr.encode_telemetry2(t)[51] == 0xFF
    # ④缺省值也不是 0：只给 `state_id` 的老字典编出来的帧，task_id 位应为 0xFF
    assert pr.encode_telemetry2({"state_id": 0xFF})[51] == 0xFF


def test_telemetry_encoder_is_inverse_of_decoder():
    t = {"t": 999, "yaw": -12.3, "pitch": 4.4, "roll": -1.2, "vl": 100, "vr": -100,
         "pwm_l": 900, "pwm_r": -900, "vbat_mv": 11900, "ibat_ma": -120,
         "servo1_us": 1500, "servo2_us": 700, "line": [1, 0, 1, 0, 0, 0, 0, 1],
         "gray": [1, 2, 3, 4, 5, 6, 7, 8], "state_id": 5, "loop_us": 300,
         "mode": 2, "task_id": 0, "action": 3, "nvram": 1, "cross": True,
         "trace": {"pos": -1.5, "pid": 42}, "angle": {"err": -0.7, "rate_ref": 1.1, "corr": 7},
         "flags": {"gyro_ok": True, "lost": True, "slope": False, "host_drive": True,
                   "motor": True, "logging": False, "flash": False, "fault": True},
         "drive": {"mode": 2, "enabled": True}, "servo1_deg": 30, "servo2_deg": 150,
         "watchdog_trips": 2, "servo_dev": {"1": 5, "2": -5}}
    back = pr.decode_telemetry(pr.encode_telemetry2(t))
    for k in ("t", "yaw", "pitch", "roll", "vl", "vr", "pwm_l", "pwm_r", "vbat_mv",
              "ibat_ma", "servo1_us", "servo2_us", "line", "gray", "state_id", "loop_us",
              "mode", "task_id", "action", "nvram", "cross", "watchdog_trips", "trace",
              "angle", "flags", "drive", "servo1_deg", "servo2_deg", "servo_dev"):
        assert back[k] == t[k], k


def test_hello_param_actions_cal_and_drive_reports():
    h = bytearray(24)
    struct.pack_into("<H", h, 0, 0x0002)
    h[2] = 2
    struct.pack_into("<H", h, 4, 0x1234)
    struct.pack_into("<H", h, 6, 50)
    h[8:18] = b"quandi-car"
    struct.pack_into("<I", h, 20, 0x7F | pr.CAP_TASK)
    hi = pr.decode_hello(bytes(h))
    assert hi["fw_raw"] == 2 and hi["version"] == "0.2.0"
    assert hi["proto"] == 2 and hi["param_crc"] == 0x1234 and hi["telem_hz"] == 50
    assert hi["name"] == "quandi-car"
    assert hi["caps"] == 0x7F | pr.CAP_TASK
    for bit in (pr.CAP_TELEM2, pr.CAP_ACTIONS, pr.CAP_SERVO, pr.CAP_DRIVE,
                pr.CAP_CAL, pr.CAP_FLASH, pr.CAP_WATCHDOG, pr.CAP_TASK):
        assert hi["caps"] & bit

    pid, val = pr.decode_param_report(pr.pack_u16(0x0207) + pr.pack_f32(900.0))
    assert pid == 0x0207 and abs(val - 900.0) < 1e-6

    acts = pr.decode_actions(bytes(range(28)))
    assert len(acts) == 14 and acts[0] == (0, 1) and acts[13] == (26, 27)

    cal = pr.decode_cal(bytes(82))
    assert len(cal["ch"]) == 8 and cal["ch"][0]["i"] == 0

    p = bytes([2, 1]) + pr.pack_i16(300) + pr.pack_i16(-300) \
        + pr.pack_i16(1200) + pr.pack_i16(-1200) + bytes([90, 45])
    dr = pr.decode_drive_report(p)
    assert dr == {"mode": 2, "enabled": True, "a": 300, "b": -300,
                  "pwm_l": 1200, "pwm_r": -1200, "servo1_deg": 90, "servo2_deg": 45}

    assert pr.decode_text(b"hello \xe4\xb8\xad\xe6\x96\x87") == "hello 中文"


def test_decode_cal_per_channel_uses_bad_array():
    """回归：`valid` 是整套 0/1 标志，逐路好坏看 `bad[]`。

    老实现写成 `valid & (1 << i)`（当作位掩码）⇒ 标定有效时也只有第 1 路报 ok，
    其余 7 路被误报 reason=2，标定页会显示「1/8 路有效」。口径要与固件的
    trace_cal_ch_ok / trace_cal_bad_reason / trace_cal_quality 一致。
    """
    p = bytearray(82)
    p[0] = 1                 # valid：整套有效
    p[9 + 0] = 0             # 第 1 路：好
    p[9 + 1] = 2             # 第 2 路：JITTER
    p[9 + 7] = 1             # 第 8 路：SPAN
    for i in range(8):
        p[18 + i * 2:20 + i * 2] = pr.pack_u16(170 + i)   # blk
        p[34 + i * 2:36 + i * 2] = pr.pack_u16(320 + i)   # wht
    cal = pr.decode_cal(bytes(p))
    assert cal["quality"] == 6
    assert [c["ok"] for c in cal["ch"]] == [True, False, True, True, True, True, True, False]
    assert [c["reason"] for c in cal["ch"]] == [0, 2, 0, 0, 0, 0, 0, 1]
    assert cal["ch"][0]["blk"] == 170 and cal["ch"][7]["bad"] == 1

    p[0] = 0                 # 整套没标定 ⇒ 全部 NOSAMPLE(3)，与固件 bad_reason 一致
    cal0 = pr.decode_cal(bytes(p))
    assert cal0["quality"] == 0 and all(c["reason"] == 3 for c in cal0["ch"])


def test_drive_and_servo_frame_builders():
    raw = pr.frame_drive(11, 2, 300, -45)
    f = pr.FrameDecoder().feed(raw)[0]
    assert f.type == pr.T_DRIVE and f.seq == 11 and f.payload[0] == 2
    assert pr.i16(f.payload, 1) == 300 and pr.i16(f.payload, 3) == -45
    raw = pr.frame_drive(12, 0)
    f = pr.FrameDecoder().feed(raw)[0]
    assert f.payload == bytes([0, 0, 0, 0, 0])
    raw = pr.frame_servo_set(13, 1, 90)
    f = pr.FrameDecoder().feed(raw)[0]
    assert f.type == pr.T_SERVO_SET and f.payload == bytes([1]) + pr.pack_i16(90)
    raw = pr.frame_servo_dev(14, 2, -30)
    f = pr.FrameDecoder().feed(raw)[0]
    assert f.type == pr.T_SERVO_DEV and f.payload == bytes([2]) + pr.pack_i16(-30)
    raw = pr.frame_servo_play(15, 0)
    assert pr.FrameDecoder().feed(raw)[0].payload == bytes([0])
    raw = pr.frame_param_set(16, 0x0207, 1200.0)
    f = pr.FrameDecoder().feed(raw)[0]
    assert pr.decode_param_report(f.payload) == (0x0207, 1200.0)
    raw = pr.frame_cmd(17, pr.CMD_SET_TELEM_HZ, 100)
    f = pr.FrameDecoder().feed(raw)[0]
    assert f.type == pr.T_CMD and f.payload == bytes([pr.CMD_SET_TELEM_HZ]) + pr.pack_i32(100)
    assert pr.decode_cmd_arg(f.payload) == 100
    raw = pr.frame_hello_req(18)
    assert pr.FrameDecoder().feed(raw)[0].type == pr.T_HELLO_REQ
    raw = pr.frame_actions_get(19)
    assert pr.FrameDecoder().feed(raw)[0].type == pr.T_ACTIONS_GET
    raw = pr.frame_action_set(20, 3, 100, 20)
    assert pr.FrameDecoder().feed(raw)[0].payload == bytes([3, 100, 20])
    raw = pr.frame_cal_get(21)
    assert pr.FrameDecoder().feed(raw)[0].type == pr.T_CAL_GET
    raw = pr.frame_param_save(22)
    assert pr.FrameDecoder().feed(raw)[0].type == pr.T_PARAM_SAVE
    raw = pr.frame_action_save(23)
    assert pr.FrameDecoder().feed(raw)[0].type == pr.T_ACTION_SAVE
    raw = pr.frame_param_get(24, 0x0301)
    assert pr.FrameDecoder().feed(raw)[0].payload == pr.pack_u16(0x0301)


def test_cmd_payload_carries_a_full_32_bit_arg():
    """T_CMD 载荷 = cmd + **4 字节小端** arg（固件 `len >= 5` 才读，见 host_link.c:776-782）。"""
    assert pr.cmd_payload(pr.CMD_SET_TELEM_HZ, 200) == bytes([pr.CMD_SET_TELEM_HZ]) + b"\xc8\x00\x00\x00"
    assert pr.decode_cmd_arg(pr.cmd_payload(pr.CMD_SET_TELEM_HZ, 200)) == 200
    assert pr.decode_cmd_arg(pr.cmd_payload(pr.CMD_SET_TELEM_MODE, -1)) == -1
    assert pr.pack_i32(1 << 40) == b"\xff\xff\xff\x7f"      # 溢出钳到 INT32_MAX，不炸
    assert pr.pack_i32(-(1 << 40)) == b"\x00\x00\x00\x80"


def test_decode_cmd_arg_accepts_the_legacy_one_byte_arg():
    """老桥只发 1 字节 arg：当 0 会把 CMD_SET_TELEM_HZ 钳成 1Hz（2026-10-04 真车踩过）。"""
    assert pr.decode_cmd_arg(bytes([pr.CMD_SET_TELEM_HZ, 50])) == 50
    assert pr.decode_cmd_arg(bytes([pr.CMD_SET_TELEM_HZ])) == 0
    assert pr.decode_cmd_arg(b"") == 0
