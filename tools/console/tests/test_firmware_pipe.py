"""真固件源码 ↔ 上位机编解码器：把固件协议层编成 PC 程序，逐字节对拍。

为什么要有这个文件：`test_protocol.py` 的遥测/回帧偏移是**照着 host_link.c 手抄**的，
固件侧 `firmware/test/t_hostlink.c` 又各自手抄了一份 —— 两份抄写彼此不认识，一边改对了、
另一边没跟上时谁都不会红。这里换一种做法：把**真** `App/Svc/host_link.c` + `App/Svc/host_ext.c`
（配 `firmware/test/stub/` 的假 HAL）编成 PC 可执行文件，用 `bridge/protocol.py` 造帧喂进去、
再用 `bridge/protocol.py` 解回来。于是"上位机以为的协议"和"固件实际吐出来的字节"
必须完全一致，否则测试红 —— 而且这个对拍是**真源码**，不是第二份手抄。

管道程序见 `firmware/test/pipe_hostlink.c`：stdin 一个字节 → `host_dl_feed()` →
`host_link_poll()`（其间推进假时钟，遥测照常发）→ `host_dl_tx()` 全量写 stdout
（二进制无缓冲；诊断只走 stderr，所以 stdout 可以整段当字节流解析）。

没有 gcc（或没装 MinGW）时整组 skip —— 这台机器上的配方与 `firmware/test/run_tests.ps1` 一致。
"""
from __future__ import annotations

import os
import shutil
import subprocess

import pytest

from bridge import params as P
from bridge import protocol as pr

from test_actions_spec import _firmware_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

GCC = os.environ.get("PIPE_GCC") or shutil.which("gcc") or r"C:\mingw64\bin\gcc.exe"

INCLUDES = ("firmware/test/stub", "firmware/Core/Inc", "firmware/App/Inc", "firmware/App/Bsp",
            "firmware/App/Svc", "firmware/App/Fonts", "firmware/Middlewares/kk_oled/include",
            "firmware/Middlewares/kk_ui/include", "firmware/Middlewares/SEGGER_RTT", "firmware/test")

# 与 run_tests.ps1 同配方：MinGW 没有 asan/ubsan 运行库，所以用 trap 模式（越界/溢出/除零/空指针
# 会直接变成非法指令把进程杀掉，不会静默算出一个假值）
# `-DHOST_PIPE_REAL_SERVO`：关掉 host_appstub.c 的假舵机，改用真 App/Bsp/servo.c
FLAGS = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error",
         "-DHOST_PIPE_REAL_SERVO"]

SOURCES = ("firmware/test/pipe_hostlink.c", "firmware/App/Svc/host_link.c",
           "firmware/App/Svc/host_ext.c", "firmware/App/Bsp/servo.c",
           "firmware/test/stub/host_appstub.c", "firmware/test/stub/host_pipestub.c",
           "firmware/test/stub/host_hal.c", "firmware/test/stub/host_board.c",
           "firmware/test/stub/host_rtt.c")


def _repo(*parts: str) -> str:
    return os.path.join(REPO, *parts)


@pytest.fixture(scope="module")
def pipe(tmp_path_factory):
    """编一次，返回 `run(*chunks, pad=64) -> list[Frame]`。"""
    if not (os.path.isfile(GCC) or shutil.which(GCC)):
        pytest.skip(f"没有 gcc，跳过真固件对拍（可用 PIPE_GCC 指定）：{GCC}")

    exe = str(tmp_path_factory.mktemp("pipe") / ("pipe_hostlink.exe" if os.name == "nt" else "pipe_hostlink"))
    cmd = ([GCC] + FLAGS + ["-I" + _repo(p) for p in INCLUDES]
           + [_repo(s) for s in SOURCES] + ["-o", exe])
    cp = subprocess.run(cmd, capture_output=True, timeout=600)
    # gcc 在中文 Windows 上按 GBK 吐诊断，用 text=True 会让读线程抛 UnicodeDecodeError、
    # stdout 变空（只看得到 returncode）⇒ 一律按 bytes 收、再容错解码
    assert cp.returncode == 0, ("固件管道程序编译失败：\n"
                                + cp.stdout.decode("utf-8", "replace")
                                + cp.stderr.decode("utf-8", "replace"))

    def run(*chunks: bytes, pad: int = 64):
        # pad 个 0x00 是"没帧的静默时间"：1 字节 = 4 拍 × 5ms = 20ms 假时钟
        raw = b"".join(chunks) + b"\x00" * pad
        p = subprocess.run([exe], input=raw, capture_output=True, timeout=180)
        assert p.returncode == 0, ("管道程序异常退出 %d（trap 模式：多半是真源码里踩了 UB）：%s"
                                   % (p.returncode, p.stderr.decode("utf-8", "replace")))
        err = p.stderr.decode("utf-8", "replace").strip()
        assert err == "", f"管道程序不该往 stderr 写东西：{err}"
        dec = pr.FrameDecoder()
        frames = dec.feed(p.stdout)
        assert dec.crc_errors == 0 and dec.resyncs == 0, \
            f"真固件发的字节流有坏 CRC/重同步：crc_errors={dec.crc_errors} resyncs={dec.resyncs}"
        return frames

    return run


def _first(frames, ftype):
    return next((f for f in frames if f.type == ftype), None)


def _last(frames, ftype):
    return next((f for f in reversed(frames) if f.type == ftype), None)


def _telem_after(frames, seq: int):
    """按 seq 找到某个回帧，返回它之后的所有 T_TELEM2（帧到达顺序即固件处理顺序）。"""
    idx = next((i for i, f in enumerate(frames) if f.seq == seq), None)
    assert idx is not None, f"没等到 seq={seq} 的回帧"
    return [pr.decode_telemetry(f.payload) for f in frames[idx:] if f.type == pr.T_TELEM2]


def test_hello_from_real_firmware(pipe):
    frames = pipe(pr.frame_hello_req(1))
    hello = _first(frames, pr.T_HELLO)
    assert hello is not None, "真固件没回 HELLO"
    h = pr.decode_hello(hello.payload)
    assert h["proto"] == 2
    assert h["version"] == "0.2.0"
    assert h["name"] == "quandi-car"
    assert h["telem_hz"] == 50
    # 7 个能力位全开（protocol.py 的常量必须和固件 host_link.c 的 CAP_* 对齐）
    assert h["caps"] == (pr.CAP_TELEM2 | pr.CAP_ACTIONS | pr.CAP_SERVO | pr.CAP_DRIVE
                         | pr.CAP_CAL | pr.CAP_FLASH | pr.CAP_WATCHDOG)
    assert _first(frames, pr.T_TELEM2) is not None, "HELLO 之后应该有 64B 遥测在流"


def test_param_dump_values_and_order_match_the_bridge_table(pipe):
    """参数 dump：id 集合、默认值、**顺序** 三项都要对得上。

    顺带证明 T_HELLO 的 param_crc 能被上位机算出来：固件按 `SPECS[]` 顺序打包
    `(id, 出厂默认 f32)` 算 CRC16；dump 回来的顺序就是 SPECS[] 顺序 ⇒ 用 dump 的
    顺序 + `bridge/params.py` 的默认值，必须复现固件报的那个 param_crc。
    这是"上位机参数表与固件参数表逐字节一致"的硬证据（比解析源码更强：用的是真二进制）。
    """
    frames = pipe(pr.frame_hello_req(1), pr.frame_param_get(2), pad=200)
    h = pr.decode_hello(_first(frames, pr.T_HELLO).payload)
    dump = [pr.decode_param_report(f.payload) for f in frames if f.type == pr.T_PARAM_REPORT]
    assert len(dump) == len(P.PARAMS) == 43, f"dump 项数 {len(dump)} 与上位机表 {len(P.PARAMS)} 不一致"

    order = [pid for pid, _ in dump]
    assert len(set(order)) == len(order), "同一项被 dump 了两次"
    assert set(order) == set(P.BY_ID), \
        f"id 集合不一致：固件多 {[hex(i) for i in set(order) - set(P.BY_ID)]}，" \
        f"上位机多 {[hex(i) for i in set(P.BY_ID) - set(order)]}"

    for pid, val in dump:
        spec = P.BY_ID[pid]
        assert abs(val - spec.default) <= max(1e-4, abs(spec.default) * 1e-5), \
            f"{spec.key}(0x{pid:04X}) 出厂默认值不一致：固件 {val} vs params.py {spec.default}"

    mine = pr.param_table_crc((pid, P.BY_ID[pid].default) for pid in order)
    assert mine == h["param_crc"], (
        f"按固件 dump 顺序算出来的参数表 CRC {mine} != 固件 HELLO 报的 {h['param_crc']}"
        "（说明上位机的 id/默认值表与固件 SPECS[] 已经不一致）")
    # 反面：params.py 的界面顺序**不能**用来算这个 CRC（顺序敏感，会得到另一个值）
    assert pr.param_table_crc((s.id, s.default) for s in P.PARAMS) != h["param_crc"]


def test_param_set_ack_report_and_range_errors(pipe):
    spec = P.BY_KEY["pivot_kp"]
    frames = pipe(pr.frame_param_set(4, spec.id, 1234.0))
    ack = _first(frames, pr.T_ACK)
    assert ack is not None and ack.seq == 4
    assert pr.decode_ack(ack.payload)["status"] == pr.ST_OK
    rep = _first(frames, pr.T_PARAM_REPORT)
    assert rep is not None and pr.decode_param_report(rep.payload) == (spec.id, 1234.0)

    bad = pipe(pr.frame_param_set(5, spec.id, spec.max * 10.0),   # 越界
               pr.frame_param_set(6, 0x7FFF, 1.0))                # 不存在的 id
    status = {f.seq: pr.decode_ack(f.payload)["status"] for f in bad if f.type == pr.T_ACK}
    assert status[5] == pr.ST_OUT_OF_RANGE, status
    assert status[6] == pr.ST_BAD_PARAM, status


def test_servo_and_drive_state_reaches_telemetry_and_drive_report(pipe):
    # pad=8 ⇒ 命令之后只有 ~160ms 假时钟，远小于 500ms 驾驶看门狗窗口
    frames = pipe(pr.frame_servo_set(7, 1, 90),
                  pr.frame_servo_dev(8, 2, -30),
                  pr.frame_drive(9, 1, 300, -300),
                  pad=8)
    rep = _first(frames, pr.T_DRIVE_REPORT)
    assert rep is not None, "T_DRIVE 之后应该有一帧 T_DRIVE_REPORT"
    dr = pr.decode_drive_report(rep.payload)
    assert (dr["mode"], dr["enabled"], dr["a"], dr["b"]) == (1, True, 300, -300)
    assert dr["servo1_deg"] == 90

    after = _telem_after(frames, 7)
    assert after, "舵机命令之后应该有遥测"
    t = after[-1]
    assert t["servo1_deg"] == 90, t["servo1_deg"]
    assert t["servo_dev"] == {"1": 0, "2": -30}, t["servo_dev"]
    assert t["drive"] == {"mode": 1, "enabled": True}, t["drive"]
    assert t["watchdog_trips"] == 0, "没超时就不该有看门狗计数"


def test_drive_watchdog_stops_the_car_when_the_link_goes_silent(pipe):
    """链路失联保护（object 里明确要的一条）：静默 > 500ms 必须自动停车并记一次 trips。

    pad=48 ⇒ 命令之后 960ms 假时钟，跨过 HOST_DRIVE_TIMEOUT_MS(500ms)。
    """
    frames = pipe(pr.frame_drive(10, 1, 300, -300), pad=48)
    after = _telem_after(frames, 10)
    assert after, "驾驶命令之后应该有遥测"
    assert after[0]["drive"] == {"mode": 1, "enabled": True}, "刚下发时驾驶应该是开着的"
    t = after[-1]
    assert t["watchdog_trips"] >= 1, f"静默 960ms 之后看门狗没记停车：{t['watchdog_trips']}"
    assert t["drive"] == {"mode": 0, "enabled": False}, f"看门狗没把驾驶停掉：{t['drive']}"


def test_action_table_equals_the_firmware_source_defaults(pipe):
    frames = pipe(pr.frame_actions_get(11), pad=80)
    f = _first(frames, pr.T_ACTIONS)
    assert f is not None, "真固件没回动作表"
    acts = pr.decode_actions(f.payload)
    assert len(acts) == pr.ACTION_COUNT == 14
    # 编译进可执行文件里的运行值 == servo.c 的 k_action_default（源码解析见 test_actions_spec）
    fw = [tuple(x) for x in _firmware_defaults()]
    assert acts == fw, f"固件运行时的动作表与 k_action_default 不一致：{acts} vs {fw}"


def test_action_pose_set_is_range_checked_by_the_real_firmware(pipe):
    """角度越界必须由**固件**挡掉（前端夹取只是体验，真值在固件）。

    T_ACTION_SET 的 a1/a2 是一个字节（0..255；负数在协议里根本表达不出来），
    `firmware/App/Svc/host_link.c:692` 只放行 ≤180 ⇒ 越界回 ST_OUT_OF_RANGE 且**不落表**。
    同时验证 180/0 这种边界值能收下并出现在动作表里（真 servo.c 的 RAM 表）。
    """
    frames = pipe(pr.frame_action_set(14, 3, 180, 0),
                  pr.frame_action_set(15, 3, 251, 0),   # 251 = -5 按字节传过去
                  pr.frame_action_set(16, 99, 10, 10),  # 动作号越界
                  pr.frame_actions_get(17), pad=80)
    status = {f.seq: pr.decode_ack(f.payload)["status"] for f in frames if f.type == pr.T_ACK}
    assert status.get(14) == pr.ST_OK, status
    assert status.get(15) == pr.ST_OUT_OF_RANGE, status
    assert status.get(16) == pr.ST_OUT_OF_RANGE, status
    acts = pr.decode_actions(_last(frames, pr.T_ACTIONS).payload)
    assert acts[2] == (180, 0), f"180/0 应被接受，实际 {acts[2]}"
    want = [tuple(x) for x in _firmware_defaults()]
    want[2] = (180, 0)
    assert acts == want, f"被拒的那两次不该改动动作表：{acts} vs {want}"


def test_servo_deviation_range_is_checked_by_the_real_firmware(pipe):
    """中位偏差硬件上限 ±SERVO_DEVIATION_MAX(100µs)：边界值要收，越界要回 ST_OUT_OF_RANGE。

    `firmware/App/Svc/host_link.c:758` 把 ±999 挡在协议层（`Servo_SetDeviation()` 里的夹取
    是第二道防线，正常路径走不到）。
    """
    frames = pipe(pr.frame_servo_dev(18, 1, 100),
                  pr.frame_servo_dev(19, 2, -100),
                  pr.frame_servo_dev(20, 1, 999), pad=8)
    status = {f.seq: pr.decode_ack(f.payload)["status"] for f in frames if f.type == pr.T_ACK}
    assert status.get(18) == pr.ST_OK and status.get(19) == pr.ST_OK, status
    assert status.get(20) == pr.ST_OUT_OF_RANGE, status
    dev = _telem_after(frames, 20)[-1]["servo_dev"]
    assert dev == {"1": 100, "2": -100}, f"±100 该收下、越界那次该被丢弃：{dev}"


def test_save_requests_are_acked(pipe):
    frames = pipe(pr.frame_action_save(12), pr.frame_param_save(13))
    status = {f.seq: pr.decode_ack(f.payload)["status"] for f in frames if f.type == pr.T_ACK}
    assert status.get(12) == pr.ST_OK and status.get(13) == pr.ST_OK, status
