"""仿真车（`--sim` 数据源）行为测试：走的是真帧协议，所以这一层也在测桥的解码。"""
from __future__ import annotations

import pytest

from bridge import params as P
from bridge import protocol as pr
from bridge.sim import ACTION_DEFAULT, SimCar, norm180


def req(car: SimCar, ftype: int, payload: bytes = b"", seq: int = 1,
        now: int = 1000) -> list[pr.Frame]:
    """发一帧给仿真车，返回它这一拍吐出来的所有帧（含遥测）。"""
    car.send(pr.build_frame(ftype, seq, payload))
    return car.poll(now)


def only(frames: list[pr.Frame], ftype: int) -> pr.Frame:
    hit = [f for f in frames if f.type == ftype]
    assert hit, f"没收到 {pr.TYPE_NAMES.get(ftype, ftype)} 帧（收到 {[pr.TYPE_NAMES.get(f.type) for f in frames]}）"
    return hit[0]


def acks(frames: list[pr.Frame]) -> list[int]:
    return [pr.decode_ack(f.payload)["status"] for f in frames if f.type == pr.T_ACK]


def test_hello_and_caps():
    car = SimCar()
    car.start(1000)
    f = only(req(car, pr.T_HELLO_REQ), pr.T_HELLO)
    h = pr.decode_hello(f.payload)
    assert h["proto"] == 2
    assert h["name"] == "quandi-car"
    assert h["caps"] == 0x7F | pr.CAP_TASK
    assert h["fw_raw"] == 0x0002
    assert h["telem_hz"] == 50


def test_telemetry_cadence_follows_hz():
    car = SimCar()
    car.start(0)
    assert only(car.poll(0), pr.T_TELEM2)                       # 第一拍就发
    assert not [f for f in car.poll(5) if f.type == pr.T_TELEM2]
    assert [f for f in car.poll(20) if f.type == pr.T_TELEM2]   # 50Hz = 20ms
    frames = req(car, pr.T_CMD, bytes([pr.CMD_SET_TELEM_HZ, 10]), now=100)
    assert 0x00 in acks(frames)
    t = car.poll(110)
    assert not [f for f in t if f.type == pr.T_TELEM2]           # 10Hz：还不到 100ms
    assert [f for f in car.poll(210) if f.type == pr.T_TELEM2]


def test_telemetry_payload_decodes():
    car = SimCar()
    car.start(0)
    f = only(car.poll(0), pr.T_TELEM2)
    assert len(f.payload) == pr.TELEM2_LEN
    t = pr.decode_telemetry(f.payload)
    assert -180 <= t["yaw"] <= 180
    assert len(t["gray"]) == 8 and all(0 <= g <= 255 for g in t["gray"])
    assert t["vbat_mv"] > 0 and 500 <= t["servo1_us"] <= 2500
    assert set(t["flags"]) >= {"gyro_ok", "lost", "host_drive", "flash", "fault"}
    assert t["mode"] in (0, 1, 2, 3) and t["nvram"] in (0, 1, 2, 3, 4)
    assert t["drive"]["mode"] in (0, 1, 2, 3)
    # 仿真车也要能编回一模一样的帧（encode/decode 互逆）
    assert pr.encode_telemetry2(t) == f.payload


def test_param_dump_has_every_param():
    car = SimCar()
    car.start(0)
    frames = req(car, pr.T_PARAM_GET, pr.pack_u16(0xFFFF))
    reps = [pr.decode_param_report(f.payload) for f in frames if f.type == pr.T_PARAM_REPORT]
    assert len(reps) == len(P.PARAMS)
    ids = {pid for pid, _ in reps}
    assert ids == set(P.DEFAULTS)
    for pid, v in reps:
        assert v == pytest.approx(P.BY_ID[pid].default)


def test_param_set_range_and_unknown_id():
    car = SimCar()
    car.start(0)
    spec = P.BY_KEY["pivot_kp"]
    frames = req(car, pr.T_PARAM_SET, pr.pack_u16(spec.id) + pr.pack_f32(1200.0), seq=5)
    assert acks(frames) == [pr.ST_OK]
    assert car.params[spec.id] == 1200.0
    assert pr.decode_param_report(only(frames, pr.T_PARAM_REPORT).payload)[1] == 1200.0

    frames = req(car, pr.T_PARAM_SET, pr.pack_u16(spec.id) + pr.pack_f32(99999.0), seq=6)
    assert acks(frames) == [pr.ST_OUT_OF_RANGE]
    assert car.params[spec.id] == 1200.0                # 越界不改

    frames = req(car, pr.T_PARAM_SET, pr.pack_u16(0x7FFF) + pr.pack_f32(1.0), seq=7)
    assert acks(frames) == [pr.ST_BAD_PARAM]
    frames = req(car, pr.T_PARAM_SET, b"\x01", seq=8)
    assert acks(frames) == [pr.ST_BAD_PARAM]


def test_action_table_and_servo():
    car = SimCar()
    car.start(0)
    frames = req(car, pr.T_ACTIONS_GET)
    assert pr.decode_actions(only(frames, pr.T_ACTIONS).payload) == [tuple(a) for a in ACTION_DEFAULT]

    frames = req(car, pr.T_ACTION_SET, bytes([3, 100, 20]), seq=11)
    assert acks(frames) == [pr.ST_OK]
    assert car.actions[2] == [100, 20]
    assert pr.decode_actions(only(frames, pr.T_ACTIONS).payload)[2] == (100, 20)
    assert [int(car.servo_deg[0]), int(car.servo_deg[1])] == [100, 20]   # 改完立刻下发
    assert car.action == 3

    assert acks(req(car, pr.T_ACTION_SET, bytes([99, 10, 10]), seq=12)) == [pr.ST_OUT_OF_RANGE]
    assert acks(req(car, pr.T_ACTION_SET, bytes([3, 200, 10]), seq=13)) == [pr.ST_OUT_OF_RANGE]
    assert car.actions[2] == [100, 20]

    assert acks(req(car, pr.T_SERVO_SET, bytes([1]) + pr.pack_i16(90), seq=14)) == [pr.ST_OK]
    assert int(car.servo_deg[0]) == 90
    assert acks(req(car, pr.T_SERVO_SET, bytes([3]) + pr.pack_i16(90), seq=15)) == [pr.ST_BAD_PARAM]
    assert acks(req(car, pr.T_SERVO_SET, bytes([2]) + pr.pack_i16(200), seq=16)) == [pr.ST_OUT_OF_RANGE]
    assert acks(req(car, pr.T_SERVO_PLAY, bytes([0]), seq=17)) == [pr.ST_OK]
    assert car.servo_deg == [0.0, 0.0] and car.action == 0
    assert acks(req(car, pr.T_SERVO_PLAY, bytes([5]), seq=18)) == [pr.ST_OK]
    assert [int(car.servo_deg[0]), int(car.servo_deg[1])] == list(ACTION_DEFAULT[4])
    assert acks(req(car, pr.T_SERVO_PLAY, bytes([99]), seq=19)) == [pr.ST_OUT_OF_RANGE]
    assert acks(req(car, pr.T_ACTION_SAVE, b"", seq=20)) == [pr.ST_OK]
    assert car.nvram == 1


def test_servo_pulse_matches_angle():
    car = SimCar()
    car.start(0)
    req(car, pr.T_SERVO_SET, bytes([1]) + pr.pack_i16(0), seq=1, now=0)
    car.poll(1)
    assert car.servo_us[0] == 500
    req(car, pr.T_SERVO_SET, bytes([1]) + pr.pack_i16(180), seq=2, now=2)
    car.poll(3)
    assert car.servo_us[0] == 2500
    t = pr.decode_telemetry(car._telem_bytes())
    assert t["servo1_us"] == 2500 and t["servo1_deg"] == 180


def test_drive_modes_and_watchdog():
    car = SimCar()
    car.start(0)
    frames = req(car, pr.T_DRIVE, bytes([1]) + pr.pack_i16(300) + pr.pack_i16(-300), seq=21, now=10)
    assert acks(frames) == [pr.ST_OK]
    d = pr.decode_drive_report(only(frames, pr.T_DRIVE_REPORT).payload)
    assert d["mode"] == 1 and d["enabled"] is True
    assert d["a"] == 300 and d["b"] == -300
    assert car.vl_t == 300 and car.vr_t == -300

    # 500ms 内没有合法帧 → 自动停车并记一次 trip
    car.poll(400)
    assert car.drive_enabled is True
    car.poll(600)
    assert car.drive_enabled is False and car.drive_trips == 1
    t = pr.decode_telemetry(car._telem_bytes())
    assert t["watchdog_trips"] == 1 and t["drive"]["enabled"] is False

    # base_turn：锁存进入时的航向 + turn
    yaw0 = car.yaw
    frames = req(car, pr.T_DRIVE, bytes([2]) + pr.pack_i16(4000) + pr.pack_i16(30), seq=22, now=700)
    assert acks(frames) == [pr.ST_OK]
    assert car.mode == 1 and car.yaw_ref == pytest.approx(yaw0 + 30, abs=1e-6)

    frames = req(car, pr.T_DRIVE, bytes([3]) + pr.pack_i16(2500) + pr.pack_i16(0), seq=23, now=720)
    assert acks(frames) == [pr.ST_OK]
    assert car.mode == 3

    assert acks(req(car, pr.T_DRIVE, bytes([9]) + pr.pack_i16(0) + pr.pack_i16(0), seq=24, now=740)) == [pr.ST_BAD_PARAM]
    assert acks(req(car, pr.T_DRIVE, bytes([1]), seq=25, now=760)) == [pr.ST_BAD_PARAM]

    frames = req(car, pr.T_DRIVE, bytes([0]) + pr.pack_i16(0) + pr.pack_i16(0), seq=26, now=780)
    assert 0x00 in acks(frames) and car.drive_enabled is False and car.mode == 0


def test_drive_refused_while_task_running():
    car = SimCar()
    car.start(0)
    assert acks(req(car, pr.T_CMD, bytes([pr.CMD_START_A]), seq=31, now=10)) == [pr.ST_OK]
    assert car.renwu == 1
    frames = req(car, pr.T_DRIVE, bytes([1]) + pr.pack_i16(300) + pr.pack_i16(300), seq=32, now=20)
    assert acks(frames) == [pr.ST_NOT_READY]
    assert any(b"drive refused" in f.payload for f in frames if f.type == pr.T_TEXT)
    assert car.drive_enabled is False
    assert acks(req(car, pr.T_CMD, bytes([pr.CMD_STOP]), seq=33, now=30)) == [pr.ST_OK]
    assert car.renwu == 0
    assert acks(req(car, pr.T_DRIVE, bytes([1]) + pr.pack_i16(0) + pr.pack_i16(0), seq=34, now=40)) == [pr.ST_OK]


def test_nvram_state_machine():
    car = SimCar()
    car.start(0)
    assert car.nvram == 0
    req(car, pr.T_PARAM_SAVE, b"", seq=41, now=0)
    assert car.nvram == 1                                   # 待保存
    car.poll(1000)
    assert car.nvram == 1
    car.poll(1600)
    assert car.nvram == 2                                   # 保存中
    car.poll(1800)
    assert car.nvram == 3                                   # 已保存
    assert pr.decode_telemetry(car._telem_bytes())["nvram"] == 3


def test_cal_report_and_unknown_frames():
    car = SimCar()
    car.start(0)
    f = only(req(car, pr.T_CAL_GET), pr.T_CAL_REPORT)
    cal = pr.decode_cal(f.payload)
    assert len(f.payload) == 82 and len(cal["ch"]) == 8
    for c in cal["ch"]:
        assert c["blk"] > c["wht"] > 0 and c["thr"] > 0 and c["ok"] is True
    assert acks(req(car, 0x77, b"", seq=42)) == [pr.ST_UNKNOWN_TYPE]
    assert acks(req(car, pr.T_CMD, bytes([0x7E]), seq=43)) == [pr.ST_BAD_CMD]


def test_yaw_reacts_to_wheel_difference():
    car = SimCar()
    car.start(0)
    req(car, pr.T_DRIVE, bytes([1]) + pr.pack_i16(-300) + pr.pack_i16(300), seq=51, now=0)
    y0 = car.yaw
    for i in range(1, 60):
        car.poll(i * 20)
    assert car.yaw > y0                                     # 右轮快 → 航向增加
    assert -180 <= car.yaw <= 180


def test_base_turn_angle_loop_converges_and_holds_base():
    """`base_turn` = "转到某个绝对航向并保持"：角度环必须真的把 yaw 拉过去
    （只锁存 yaw_ref 不闭环的话，仿真里朝向指令看起来毫无作用），而且
    重复帧（桥每 200ms 的保活）不能把基准航向拖着跟车跑。"""
    car = SimCar()
    car.start(0)
    yaw0 = car.yaw
    req(car, pr.T_DRIVE, bytes([2]) + pr.pack_i16(300) + pr.pack_i16(30), seq=61, now=0)
    assert car.mode == 1 and car.drive_enabled is True
    assert car.base_speed == 300.0
    assert car.yaw_base == pytest.approx(yaw0, abs=1e-6)          # 基准 = 进入时的航向
    assert car.yaw_ref == pytest.approx(norm180(yaw0 + 30.0), abs=1e-6)
    assert car.vr_t > car.vl_t                                    # 目标在左 ⇒ 右轮快（yaw 增大）

    seq = 62
    for i in range(1, 601):                                       # 3s，每 5ms 一拍
        now = i * 5
        if i % 40 == 0:                                           # 桥的 200ms 保活
            req(car, pr.T_DRIVE_REPORT, b"", seq=seq, now=now)
            seq += 1
        car.poll(now)
    assert car.drive_enabled is True                              # 保活 → 看门狗不咬
    assert car.drive_trips == 0
    assert car.yaw == pytest.approx(norm180(yaw0 + 30.0), abs=3.0)
    assert abs(norm180(car.yaw_ref - car.yaw)) < 3.0

    # 再来一帧同样的目标（保活里带目标的情况）→ 基准不动、目标不动
    req(car, pr.T_DRIVE, bytes([2]) + pr.pack_i16(300) + pr.pack_i16(30), seq=seq, now=3100)
    seq += 1
    assert car.yaw_base == pytest.approx(yaw0, abs=1e-6)
    assert car.yaw_ref == pytest.approx(norm180(yaw0 + 30.0), abs=1e-6)

    # 停车交还：清零并退出角度环
    req(car, pr.T_DRIVE, bytes([0]) + pr.pack_i16(0) + pr.pack_i16(0), seq=seq, now=3200)
    seq += 1
    assert car.mode == 0 and car.drive_enabled is False
    assert car.base_speed == 0.0 and car.vl_t == 0.0 and car.vr_t == 0.0

    # 重新接管：基准航向按**这一刻**重新锁存（与固件 s_mode!=BASE 就重锁一致）
    car.poll(3210)                                                # 先让物理走一拍
    yaw1 = car.yaw
    req(car, pr.T_DRIVE, bytes([2]) + pr.pack_i16(0) + pr.pack_i16(-20), seq=seq, now=3220)
    assert car.yaw_base == pytest.approx(yaw1, abs=1e-6)
    assert car.yaw_ref == pytest.approx(norm180(yaw1 - 20.0), abs=1e-6)
