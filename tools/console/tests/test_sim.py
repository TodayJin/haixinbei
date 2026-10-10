"""仿真车（`--sim` 数据源）行为测试：走的是真帧协议，所以这一层也在测桥的解码。"""
from __future__ import annotations

import json
import os

import pytest

from bridge import params as P
from bridge import protocol as pr
from bridge.sim import ACTION_DEFAULT, SimCar, norm180

HERE = os.path.dirname(os.path.abspath(__file__))


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
    assert h["fw_raw"] == 0x0003
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


def test_telemetry_rate_is_not_quantized_to_poll_step():
    """轮询步长（6ms）比遥测周期（10ms）小时，帧率不该被拉到轮询频率上。

    曾经 `next_tx = now_ms + 周期`（每拍从"现在"重算）⇒ 步长 6ms 时会稳定退化成
    12ms 一帧（要 100Hz 只出 ~83 帧/s）。改成累加后平均间隔 ≈10ms。
    """
    car = SimCar()
    car.start(0)
    req(car, pr.T_CMD, bytes([pr.CMD_SET_TELEM_HZ, 100]), now=0)
    n = 0
    for now in range(0, 1000, 6):                       # 0,6,…,996 共 167 拍
        n += len([f for f in car.poll(now) if f.type == pr.T_TELEM2])
    assert 94 <= n <= 102, f"1s 内只出了 {n} 帧（旧实现约 83）"


def test_cmd_arg_is_read_from_a_five_byte_payload():
    """和固件同口径：载荷 ≥5 字节时 arg = p[1..4] 小端；老式 2 字节载荷取低字节。"""
    car = SimCar()
    car.start(0)
    frames = req(car, pr.T_CMD, pr.cmd_payload(pr.CMD_SET_TELEM_HZ, 200), now=0)
    assert 0x00 in acks(frames)
    assert car.hz == 200
    frames = req(car, pr.T_CMD, bytes([pr.CMD_SET_TELEM_HZ, 50]), now=100)
    assert 0x00 in acks(frames)
    assert car.hz == 50, "老桥的 1 字节 arg 也要认，当成 0 会被钳成 1Hz"


def test_long_stall_does_not_burst_catch_up():
    """卡了很久之后只补一帧并重新对齐，不吐一串补帧。"""
    car = SimCar()
    car.start(0)
    assert [f for f in car.poll(0) if f.type == pr.T_TELEM2]
    assert len([f for f in car.poll(1000) if f.type == pr.T_TELEM2]) == 1
    assert len([f for f in car.poll(1010) if f.type == pr.T_TELEM2]) == 0   # 重新对齐到 1020


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


def test_telemetry_task_fields_are_ff_when_idle():
    # 语义变更：state_id = 解释器状态号、task_id = 当前任务号；没在跑时两者都是 0xFF（不是 0）
    car = SimCar()
    car.start(0)
    t = pr.decode_telemetry(only(car.poll(0), pr.T_TELEM2).payload)
    assert t["state_id"] == 0xFF and t["task_id"] == 0xFF


def test_cmd_start_a_b_c_are_unsupported_now():
    # 固件删了内置任务流程 ⇒ 这三个命令不再有作用，仿真车必须模拟真固件（回 0x08）
    car = SimCar()
    car.start(0)
    before = car.task_run["running"]
    for i, c in enumerate((pr.CMD_START_A, pr.CMD_START_B, pr.CMD_START_C)):
        frames = req(car, pr.T_CMD, bytes([c]), seq=21 + i, now=100 + i)
        assert acks(frames) == [pr.ST_UNSUPPORTED]
        assert any(b"\xe5\xb7\xb2\xe5\xba\x9f\xe5\xbc\x83" in f.payload
                   for f in frames if f.type == pr.T_TEXT), "该回一条说明文本"
    assert car.task_run["running"] is before is False
    t = pr.decode_telemetry(only(car.poll(150), pr.T_TELEM2).payload)
    assert t["task_id"] == 0xFF


def test_drive_refused_while_task_running():
    car = SimCar()
    car.start(1000)
    # 内置任务流程已删 ⇒ 只能走真的 T_TASK_PUT/COMMIT 把任务烧进去（START_A 已不生效）
    with open(os.path.join(HERE, "fixtures", "task.golden.json"), encoding="utf-8") as f:
        g = json.load(f)
    img, off, seq = bytes.fromhex(g["hex"]), 0, 31
    while off < len(img):
        chunk = img[off:off + pr.TASK_PUT_MAX]
        assert acks(req(car, pr.T_TASK_PUT, pr.pack_u16(0) + pr.pack_u16(off) + chunk,
                        seq=seq, now=1000 + seq)) == [pr.ST_OK]
        off += len(chunk)
        seq += 1
    assert acks(req(car, pr.T_TASK_COMMIT, pr.payload_task_commit(0, len(img), g["crc16"]),
                    seq=seq, now=1000 + seq)) == [pr.ST_OK]

    assert acks(req(car, pr.T_TASK_RUN, pr.pack_u16(0), seq=40, now=2000)) == [pr.ST_OK]
    assert car.task_run["running"] is True and car.task_run["id"] == 0
    t = pr.decode_telemetry(only(car.poll(2020), pr.T_TELEM2).payload)
    assert t["task_id"] == 0 and t["state_id"] == car.task_run["state"], "遥测要反映正在跑的任务"

    frames = req(car, pr.T_DRIVE, bytes([1]) + pr.pack_i16(300) + pr.pack_i16(300), seq=32, now=2100)
    assert acks(frames) == [pr.ST_NOT_READY]
    assert any(b"drive refused" in f.payload for f in frames if f.type == pr.T_TEXT)
    assert car.drive_enabled is False

    # CMD_STOP 现在也能停住正在跑的任务：停车 + task_id 回 0xFF
    assert acks(req(car, pr.T_CMD, bytes([pr.CMD_STOP]), seq=33, now=2200)) == [pr.ST_OK]
    assert car.task_run["running"] is False
    t = pr.decode_telemetry(only(car.poll(2220), pr.T_TELEM2).payload)
    assert t["task_id"] == 0xFF and t["state_id"] == 0xFF, "停了以后两个字段都要回没在跑"
    assert acks(req(car, pr.T_DRIVE, bytes([1]) + pr.pack_i16(0) + pr.pack_i16(0), seq=34, now=2300)) == [pr.ST_OK]


def test_flash_write_refused_while_task_running():
    # 擦一页会把 CPU 按住几十毫秒（正在跑的控制环直接失控）⇒ 解释器在跑时 COMMIT/DEL/RUN
    # 必须回 ST_BUSY，与固件 host_link.c 的守卫同规则。"悄悄帮你停掉任务再写/删"比拒绝更坏：
    # 操作员会以为删的是个死任务。分片 PUT 只写 RAM 暂存，车在动也允许。
    car = SimCar()
    car.start(1000)
    with open(os.path.join(HERE, "fixtures", "task.golden.json"), encoding="utf-8") as f:
        g = json.load(f)
    img = bytes.fromhex(g["hex"])

    def stage(seq0, now0):
        seq, off = seq0, 0
        while off < len(img):
            chunk = img[off:off + pr.TASK_PUT_MAX]
            assert acks(req(car, pr.T_TASK_PUT, pr.pack_u16(0) + pr.pack_u16(off) + chunk,
                            seq=seq, now=now0 + seq)) == [pr.ST_OK]
            off += len(chunk)
            seq += 1
        return seq

    seq = stage(31, 1000)
    assert acks(req(car, pr.T_TASK_COMMIT, pr.payload_task_commit(0, len(img), g["crc16"]),
                    seq=seq, now=1100)) == [pr.ST_OK]
    assert acks(req(car, pr.T_TASK_RUN, pr.pack_u16(0), seq=seq + 1, now=2000)) == [pr.ST_OK]
    assert car.task_run["running"] is True

    # 跑着的时候：合法数据 + 正确 CRC 也一律拒
    seq = stage(70, 3000)
    assert acks(req(car, pr.T_TASK_COMMIT, pr.payload_task_commit(0, len(img), g["crc16"]),
                    seq=seq, now=3100)) == [pr.ST_BUSY]
    assert acks(req(car, pr.T_TASK_DEL, pr.pack_u16(0), seq=seq + 1, now=3200)) == [pr.ST_BUSY]
    assert acks(req(car, pr.T_TASK_RUN, pr.pack_u16(0), seq=seq + 2, now=3300)) == [pr.ST_BUSY]
    assert 0 in car.tasks, "被拒的 DELETE 不许真把任务删掉"
    assert car.task_run["running"] is True, "被拒的写/删不许偷偷把任务停掉"

    # 停下来以后恢复正常
    assert acks(req(car, pr.T_CMD, bytes([pr.CMD_STOP]), seq=seq + 3, now=3400)) == [pr.ST_OK]
    assert acks(req(car, pr.T_TASK_COMMIT, pr.payload_task_commit(0, len(img), g["crc16"]),
                    seq=seq + 4, now=3500)) == [pr.ST_OK]
    assert acks(req(car, pr.T_TASK_DEL, pr.pack_u16(0), seq=seq + 5, now=3600)) == [pr.ST_OK]


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
