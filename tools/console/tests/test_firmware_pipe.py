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

import json
import os
import re
import shutil
import subprocess

import pytest

from bridge import params as P
from bridge import protocol as pr

from test_actions_spec import _firmware_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def _fw_nvram_version() -> int:
    """固件源码里的 NVRAM_VERSION（payload 布局的代次）。

    往 payload 末尾加字段时必须连它一起 +1（见 nvram.c 的 nvram_init 注释），
    所以这里跟着源码走而不是写死 —— 写死的话每加一个设置项都会变成一次假失败。
    """
    with open(os.path.join(REPO, "firmware", "App", "Svc", "nvram.c"),
              encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r"#define\s+NVRAM_VERSION\s+(\d+)U", src)
    assert m, "firmware/App/Svc/nvram.c 里找不到 NVRAM_VERSION"
    return int(m.group(1))

GCC = os.environ.get("PIPE_GCC") or shutil.which("gcc") or r"C:\mingw64\bin\gcc.exe"

INCLUDES = ("firmware/test/stub", "firmware/Core/Inc", "firmware/App/Inc", "firmware/App/Bsp",
            "firmware/App/Svc", "firmware/App/Fonts", "firmware/Middlewares/kk_oled/include",
            "firmware/Middlewares/kk_ui/include", "firmware/Middlewares/SEGGER_RTT", "firmware/test")

# 与 run_tests.ps1 同配方：MinGW 没有 asan/ubsan 运行库，所以用 trap 模式（越界/溢出/除零/空指针
# 会直接变成非法指令把进程杀掉，不会静默算出一个假值）
# `-DHOST_PIPE_REAL_SERVO`：关掉 host_appstub.c 的假舵机，改用真 App/Bsp/servo.c
# `-DHOST_PIPE_REAL_NVRAM`：关掉假 nvram_touch，改用真 App/Svc/nvram.c + trace_cal.c，
# 于是 T_PARAM_SAVE 真的会写（假）Flash —— 掉电重启闭环见 test_param_save_survives_a_power_cycle
FLAGS = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error",
         "-DHOST_PIPE_REAL_SERVO", "-DHOST_PIPE_REAL_NVRAM"]

# `App/Svc/task_store.c` / `task_prog.c` / `task_run.c` 是**真**的任务存储与解释器：
# host_link.c 的任务帧（0x60..0x69）会调它们，所以它们必须在这一串里 —— 否则管道连链接
# 都过不了（undefined reference to `TaskStore_*`/`TaskRun_*`）。用真的而不是替身，是为了让
# 下面那组任务用例走真 Flash 语义（擦页/双字编程/目录页重排都经过 host_hal 的假 Flash）。
SOURCES = ("firmware/test/pipe_hostlink.c", "firmware/App/Svc/host_link.c",
           "firmware/App/Svc/host_ext.c", "firmware/App/Bsp/servo.c",
           "firmware/App/Svc/nvram.c", "firmware/App/Svc/trace_cal.c",
           "firmware/App/Svc/task_store.c", "firmware/App/Svc/task_prog.c",
           "firmware/App/Svc/task_run.c",
           "firmware/test/stub/host_appstub.c", "firmware/test/stub/host_pipestub.c",
           "firmware/test/stub/host_hal.c", "firmware/test/stub/host_board.c",
           "firmware/test/stub/host_rtt.c")


def _repo(*parts: str) -> str:
    return os.path.join(REPO, *parts)


@pytest.fixture(scope="module")
def pipe_exe(tmp_path_factory):
    """编一次，返回可执行文件路径（要自己带命令行参数跑就直接用它）。"""
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
    return exe


def _run_exe(exe, chunks, pad: int = 64, args=()):
    """喂 `chunks` + pad 个 0x00 静默字节，解回帧列表。"""
    # pad 个 0x00 是"没帧的静默时间"：1 字节 = 4 拍 × 5ms = 20ms 假时钟
    raw = b"".join(chunks) + b"\x00" * pad
    p = subprocess.run([exe] + list(args), input=raw, capture_output=True, timeout=180)
    assert p.returncode == 0, ("管道程序异常退出 %d（trap 模式：多半是真源码里踩了 UB）：%s"
                               % (p.returncode, p.stderr.decode("utf-8", "replace")))
    err = p.stderr.decode("utf-8", "replace").strip()
    assert err == "", f"管道程序不该往 stderr 写东西：{err}"
    dec = pr.FrameDecoder()
    frames = dec.feed(p.stdout)
    assert dec.crc_errors == 0 and dec.resyncs == 0, \
        f"真固件发的字节流有坏 CRC/重同步：crc_errors={dec.crc_errors} resyncs={dec.resyncs}"
    return frames


@pytest.fixture(scope="module")
def pipe(pipe_exe):
    """返回 `run(*chunks, pad=64) -> list[Frame]`（不带命令行参数）。"""
    def run(*chunks: bytes, pad: int = 64):
        return _run_exe(pipe_exe, chunks, pad=pad)
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
    # 固件默认遥测 200Hz（2026-10-05 定档：链路提到 921600 后 200Hz 只占约 15% 带宽，
    # 且 5ms 一格正好跟 200Hz 的控制环同拍；改默认值要连这条断言一起改）
    assert h["telem_hz"] == 200
    # 8 个能力位全开（protocol.py 的常量必须和固件 host_link.c 的 CAP_* 对齐）
    assert h["caps"] == (pr.CAP_TELEM2 | pr.CAP_ACTIONS | pr.CAP_SERVO | pr.CAP_DRIVE
                         | pr.CAP_CAL | pr.CAP_FLASH | pr.CAP_WATCHDOG | pr.CAP_TASK)
    assert _first(frames, pr.T_TELEM2) is not None, "HELLO 之后应该有 64B 遥测在流"


def test_cmd_arg_is_read_from_the_payload(pipe):
    """真固件读 T_CMD 的 arg：载荷 ≥5 字节 = 4 字节小端；老式 2 字节载荷取低字节。

    2026-10-04 真车踩过：老桥只发 cmd + 1 字节 arg（载荷 2 字节），固件 `len >= 5` 不成立
    ⇒ arg 恒为 0 ⇒ `CMD_SET_TELEM_HZ` 被 `arg<1 → 1` 钳成 1Hz，车上遥测从 75 帧/s 掉到
    1 帧/s（网页一拖"遥测频率"滑块链路就哑了）。这里用真源码把两种宽度都钉住。
    """
    frames = pipe(pr.frame_cmd(1, pr.CMD_SET_TELEM_HZ, 200), pr.frame_hello_req(2))
    assert _first(frames, pr.T_ACK) is not None, "真固件没回 ACK"
    h = pr.decode_hello(_last(frames, pr.T_HELLO).payload)
    assert h["telem_hz"] == 200, "5 字节载荷里的 arg 没被固件读到"

    legacy = pr.build_frame(pr.T_CMD, 1, bytes([pr.CMD_SET_TELEM_HZ, 50]))
    frames = pipe(legacy, pr.frame_hello_req(2))
    h = pr.decode_hello(_last(frames, pr.T_HELLO).payload)
    assert h["telem_hz"] == 50, "老上位机的 1 字节 arg 不能被当成 0（会被钳成 1Hz）"


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
    assert len(dump) == len(P.PARAMS) == 45, f"dump 项数 {len(dump)} 与上位机表 {len(P.PARAMS)} 不一致"

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


def test_param_save_survives_a_power_cycle(pipe_exe, tmp_path):
    """上位机点「固化」→ 真 nvram 落盘 → 断电重启后参数还是新值（协议层闭环）。

    管道构建链了真 `App/Svc/nvram.c`（`-DHOST_PIPE_REAL_NVRAM`），所以 T_PARAM_SAVE 走的是
    真路径：`host_ext.c:226 nvram_touch()` → `nvram_poll/service` → 假 Flash 页 63
    （0x0801F800）。两个进程之间**只交换页 63 的 2048 字节**（--dump-flash / --flash），
    第二个进程的 RAM 参数表是全新的 ⇒ 还能 dump 出新值只可能是从 Flash 盖回来的。

    这条和 `firmware/test/t_nvram.c`（模块级）互补：那边直接调 nvram API，这边走真协议字节。
    """
    flash = str(tmp_path / "page63.bin")
    spec = P.BY_KEY["pivot_kd"]
    saved = 321.0

    # 第 1 次"开机"：改参数 + 点固化（pad=8 ⇒ 只给 160ms；够不着 1.5s 自动防抖，
    # 所以下面的写入只可能来自"固化"这个动作本身）
    f1 = _run_exe(pipe_exe,
                  [pr.frame_hello_req(1),
                   pr.frame_param_set(2, spec.id, saved),
                   pr.frame_param_save(3)],
                  pad=8, args=["--dump-flash", flash])
    status = {f.seq: pr.decode_ack(f.payload)["status"] for f in f1 if f.type == pr.T_ACK}
    assert status.get(2) == pr.ST_OK and status.get(3) == pr.ST_OK, status
    assert os.path.isfile(flash), "固化之后没写出 Flash 镜像 ⇒ nvram 根本没落盘"
    assert os.path.getsize(flash) == 2048
    with open(flash, "rb") as fh:
        blob = fh.read()
    # 记录头：magic "HXB1"（0x31425848）+ version u16 = 固件源码里的 NVRAM_VERSION
    assert blob[:4] == b"HXB1", f"Flash 里不是 nvram 记录：{blob[:8].hex()}"
    assert int.from_bytes(blob[4:6], "little") == _fw_nvram_version(), \
        f"nvram 记录版本与 nvram.c 的 NVRAM_VERSION 不一致：{int.from_bytes(blob[4:6], 'little')}"

    def param_dump(args):
        frames = _run_exe(pipe_exe, [pr.frame_hello_req(1), pr.frame_param_get(2, 0xFFFF)],
                          pad=200, args=args)
        return {pid: val for pid, val in
                (pr.decode_param_report(f.payload) for f in frames if f.type == pr.T_PARAM_REPORT)}

    # 第 2 次"开机"：带上 Flash 镜像 ⇒ 参数表被 Flash 里的记录盖回来
    restored = param_dump(["--flash", flash])
    assert restored.get(spec.id) == pytest.approx(saved), (
        f"掉电重启后 {spec.key}(0x{spec.id:04X}) 不是固化过的 {saved}，"
        f"而是 {restored.get(spec.id)}（Flash 里的记录没生效）")

    # 对照组：不带镜像开机 ⇒ 回到出厂默认（证明上面那个值确实来自那个 Flash 文件）
    fresh = param_dump([])
    assert fresh.get(spec.id) == pytest.approx(spec.default), \
        f"空白 Flash 开机应该是出厂默认 {spec.default}，实际 {fresh.get(spec.id)}"


def test_param_change_alone_does_not_touch_flash(pipe_exe, tmp_path):
    """只改参数、没点固化、也还没到 1.5s 自动防抖 ⇒ Flash 一个字节都不许动。

    上一条的对照组：写进 Flash 的是"固化"这个动作，而不是"跑够时间自己就写了"。
    防抖窗口见 `firmware/App/Svc/nvram.c` 的 `NVRAM_SAVE_DELAY_MS 1500`；这里静默
    800ms（pad=40）就 EOF，管道程序的收尾也只把**已排队**的固化跑完。
    """
    flash = str(tmp_path / "untouched.bin")
    spec = P.BY_KEY["pivot_kd"]

    frames = _run_exe(pipe_exe, [pr.frame_param_set(2, spec.id, 321.0)],
                      pad=40, args=["--dump-flash", flash])
    status = {f.seq: pr.decode_ack(f.payload)["status"] for f in frames if f.type == pr.T_ACK}
    assert status.get(2) == pr.ST_OK, status

    with open(flash, "rb") as fh:
        blob = fh.read()
    assert blob == b"\xFF" * 2048, "没点固化（也没到 1.5s 防抖）时 Flash 记录页被动了"


# =====================================================================
#  任务程序（T_TASK_* 0x60..0x69）：桥组帧 → 真固件 → 桥解回来
# =====================================================================
#
# `firmware/test/t_hostlink.c` 那一半用的是**手写字节**（它连的是任务存储的替身），
# 这里换真路径：镜像由上位机的编码器（`web/src/lib/task.ts`，经 tests/c/taskgen.c
# 落地成 `fixtures/task.golden.json`）产出，帧由**桥自己的** `frame_task_*` 组出来，
# 而车是**真** `host_link.c` + 真 `task_store.c`/`task_run.c`（配假 HAL 的假 Flash）。
# 三边任意一处口径漂了，这一组就红。

TASK_GOLDEN = os.path.join(HERE, "fixtures", "task.golden.json")


@pytest.fixture(scope="module")
def task_image() -> bytes:
    """上位机编出来的一整个任务镜像。顺带验证 bridge 的 CRC16 与固件同口径。"""
    with open(TASK_GOLDEN, "r", encoding="utf-8") as fh:
        d = json.load(fh)
    img = bytes.fromhex(d["hex"])
    assert len(img) == d["len"], f"金样本长度对不上：{len(img)} vs {d['len']}"
    assert pr.crc16_ccitt(img[12:]) == d["crc16"], \
        f"bridge 的 CRC16 与金样本的 {d['crc16']:04X} 不一致（车上 COMMIT 会拒收）"
    return img


def _acks(frames) -> dict:
    return {f.seq: pr.decode_ack(f.payload)["status"] for f in frames if f.type == pr.T_ACK}


def _upload(img: bytes, tid: int = 0, seq0: int = 10, name: str = "直行两米"):
    """按桥的方式分片上传 + COMMIT，返回 (帧列表, PUT 的 seq 列表, COMMIT 的 seq)。"""
    crc = pr.crc16_ccitt(img[12:])
    seq = seq0
    out, puts = [], []
    for off in range(0, len(img), pr.TASK_PUT_MAX):
        out.append(pr.frame_task_put(seq, tid, off, img[off:off + pr.TASK_PUT_MAX]))
        puts.append(seq)
        seq += 1
    out.append(pr.frame_task_commit(seq, tid, len(img), crc, name))
    return out, puts, seq


def test_task_upload_list_get_delete_round_trip(pipe, task_image):
    """PUT 分片 → COMMIT → LIST → GET → DEL → LIST，一条链在一个进程里走完。

    这里钉住三个**约定**（文档 §5）：目录用"一帧空载荷"收尾、读回用"一帧空数据"收尾、
    两者成功都不回 ACK（ACK 只在写/删/跑这些动作上有）。
    """
    img = task_image
    crc = pr.crc16_ccitt(img[12:])
    up, puts, commit_seq = _upload(img)
    frames = pipe(*up,
                  pr.frame_task_list(50),
                  pr.frame_task_get(51, 0),
                  pr.frame_task_del(52, 0),
                  pr.frame_task_list(53),
                  pad=200)

    ack = _acks(frames)
    assert all(ack.get(s) == pr.ST_OK for s in puts), f"有的 PUT 没被 ST_OK 收下：{ack}"
    assert ack.get(commit_seq) == pr.ST_OK, f"COMMIT 没成功：{ack}"
    assert ack.get(52) == pr.ST_OK, f"DEL 没成功：{ack}"
    assert 50 not in ack and 51 not in ack and 53 not in ack, \
        f"LIST/GET 成功不该回 ACK（收尾帧本身就是应答）：{ack}"

    items = [pr.decode_task_list_r(f.payload) for f in frames if f.type == pr.T_TASK_LIST_R]
    assert len(items) == 3, f"两次 LIST 该回 3 帧（条目 + 收尾 + 收尾），实际 {len(items)}：{items}"
    assert items[1] is None and items[2] is None, f"目录没有用空载荷收尾：{items}"
    assert items[0] is not None, f"第一次 LIST 没回条目：{items}"
    assert (items[0]["id"], items[0]["len"], items[0]["crc"], items[0]["flags"]) == \
        (0, len(img), crc, 0), items[0]
    assert items[0]["name"] == "直行两米", f"名字没存住：{items[0]['name']!r}"

    got = bytearray()
    end = None
    for f in frames:
        if f.type != pr.T_TASK_DATA:
            continue
        tid, off, data = pr.decode_task_data(f.payload)
        assert tid == 0, f"回读的槽号不对：{tid}"
        if not data:
            end = off
            break
        assert off == len(got), f"回读分片顺序不对：off={off} 期望 {len(got)}"
        got += data
    assert end == len(img), f"回读没收尾（end={end}）"
    assert bytes(got) == img, "GET 回来的镜像与上位机编的不是同一串字节"


def test_task_run_status_stop(pipe, task_image):
    """RUN 之后必须有 STATUS 说在跑，STOP 之后必须说停了（状态变化即推）。"""
    up, _, commit_seq = _upload(task_image)
    frames = pipe(*up, pr.frame_task_run(60, 0), pr.frame_task_stop(61), pad=200)

    ack = _acks(frames)
    assert ack.get(commit_seq) == pr.ST_OK, ack
    assert ack.get(60) == pr.ST_OK, f"RUN 被拒了：{ack}"
    assert ack.get(61) == pr.ST_OK, f"STOP 被拒了：{ack}"

    st = [pr.decode_task_status(f.payload) for f in frames if f.type == pr.T_TASK_STATUS]
    assert st, "任务状态一帧都没推"
    assert st[0]["id"] == 0xFFFF and st[0]["state"] == 0xFF, \
        f"开机第一帧 STATUS 应该是「没载程序」：{st[0]}"
    running = [s for s in st if s["running"]]
    assert running, f"RUN 之后没有一帧 STATUS 说在跑：{st}"
    assert running[0]["id"] == 0 and running[0]["state"] == 0, \
        f"在跑的应该是槽 0 的主状态：{running[0]}"
    assert st[-1]["running"] is False, f"STOP 之后最后一帧 STATUS 还在跑：{st[-1]}"


def test_task_frame_error_paths_match_the_bridge(pipe, task_image):
    """坏槽号 / 空槽 / 没收全 / CRC 不对 / 名字超长 / 载荷太短 —— 每条都得是桥认得的状态码。"""
    img = task_image
    crc = pr.crc16_ccitt(img[12:])
    up, _, _ = _upload(img)
    bad_name = (pr.pack_u16(0) + pr.pack_u16(len(img)) + pr.pack_u16(crc)
                + bytes([31]) + b"a" * 31)                       # name_len 31 > 30
    frames = pipe(up[0],                                           # 只发第一片，故意不收全
                  pr.frame_task_commit(71, 0, len(img), crc),
                  pr.frame_task_commit(72, 0, 120, pr.crc16_ccitt(img[12:120]) ^ 0xFFFF),
                  pr.build_frame(pr.T_TASK_COMMIT, 73, bad_name),
                  pr.frame_task_put(74, 99, 0, b"\x00" * 8),     # 槽号越界
                  pr.build_frame(pr.T_TASK_PUT, 75, pr.pack_u16(0) + pr.pack_u16(0)),
                  pr.frame_task_get(76, 3),                      # 空槽
                  pr.frame_task_get(77, 99),                     # 槽号越界
                  pr.frame_task_run(78, 3),                      # 空槽
                  pr.frame_task_del(79, 3),                      # 空槽：幂等
                  pr.frame_task_list(80),
                  pad=200)

    ack = _acks(frames)
    assert ack.get(71) == pr.ST_BAD_PARAM, f"没上传完就 COMMIT 该是 BAD_PARAM：{ack}"
    assert ack.get(72) == pr.ST_BAD_PARAM, f"CRC 不对该是 BAD_PARAM：{ack}"
    assert ack.get(73) == pr.ST_BAD_PARAM, f"名字超长该是 BAD_PARAM：{ack}"
    assert ack.get(74) == pr.ST_OUT_OF_RANGE, f"槽号越界该是 OUT_OF_RANGE：{ack}"
    assert ack.get(75) == pr.ST_BAD_PARAM, f"PUT 载荷不足 5 字节该是 BAD_PARAM：{ack}"
    assert ack.get(76) == pr.ST_NOT_READY, f"空槽 GET 该是 NOT_READY：{ack}"
    assert ack.get(77) == pr.ST_OUT_OF_RANGE, f"GET 槽号越界该是 OUT_OF_RANGE：{ack}"
    assert ack.get(78) == pr.ST_NOT_READY, f"空槽 RUN 该是 NOT_READY：{ack}"
    assert ack.get(79) == pr.ST_OK, f"空槽 DEL 是幂等 ST_OK：{ack}"
    items = [pr.decode_task_list_r(f.payload) for f in frames if f.type == pr.T_TASK_LIST_R]
    assert items and all(i is None for i in items), \
        f"只发了一片、没 COMMIT 成功，目录必须还是空的：{items}"


def test_task_run_refused_while_host_is_driving(pipe, task_image):
    """上位机正在驾驶时不许起任务（与 T_DRIVE 同一条铁律），且要在链路里说清楚。"""
    up, _, _ = _upload(task_image)
    frames = pipe(*up, pr.frame_drive(90, 1, 300, -300), pr.frame_task_run(91, 0), pad=200)

    ack = _acks(frames)
    assert ack.get(91) == pr.ST_BUSY, f"边驾驶边起任务该是 BUSY：{ack}"
    texts = [pr.decode_text(f.payload) for f in frames if f.type == pr.T_TEXT]
    assert any("host driving" in t for t in texts), \
        f"司机占着车时起任务要在链路上说明白（T_TEXT）：{texts}"


# ---------------------------------------------------------------------
#  旧内置比赛流程（已删的 `renwu.c`，12 个障碍）转出来的**长任务**：
#  `web/src/lib/tasks/renwu-lap.json`（15 状态 / 190 块 / 2127 字节）。
#  这是"用户真会去烧的那一份"，所以也放进真固件里走一遍：
#  ① Python 独立编一遍码（与 task.ts 逐字段对齐）⇒ 长度/CRC 两边必须一致；
#  ② 真 `host_link.c` + 真 `task_store.c` 必须**收下**它（旧的 2048 上限会拒收，
#     这正是把 TASK_STORE_CAP 提到 4096 的原因）；
#  ③ 目录里的 len/crc/名字对得上，GET 逐字节回读一致。
# ---------------------------------------------------------------------

LAP_JSON = os.path.join(HERE, "..", "web", "src", "lib", "tasks", "renwu-lap.json")
TASK_HDR_LEN, TASK_STATE_LEN, TASK_BLOCK_LEN = 12, 8, 10   # 与 firmware/App/Svc/task_prog.h 同口径
LAP_LEN = 2127                     # 与 `renwu-lap.test.ts` 里钉的字节数一致


def _encode_prog(prog: dict) -> bytes:
    """按 `web/src/lib/task.ts` 的 v1 布局编码（Python 侧独立实现一遍，用于交叉验证）。"""
    states = prog["states"]
    n = len(states)
    off = TASK_HDR_LEN + n * TASK_STATE_LEN
    eoff, boff, noff, names = [], [], [], []
    for st in states:
        eoff.append(off if st["enter"] else 0)
        off += len(st["enter"]) * TASK_BLOCK_LEN
        boff.append(off if st["body"] else 0)
        off += len(st["body"]) * TASK_BLOCK_LEN
    for st in states:
        nb = st["name"].encode("utf-8")
        names.append(nb)
        noff.append(off if nb else 0)
        off += len(nb)

    out = bytearray(off)

    def u16(at: int, v: int) -> None:
        out[at:at + 2] = (v & 0xFFFF).to_bytes(2, "little")

    def i16(at: int, v: int) -> None:
        out[at:at + 2] = max(-32768, min(32767, v)).to_bytes(2, "little", signed=True)

    out[0:2] = b"TK"
    out[2] = 1
    out[3] = n
    out[4] = prog["main"]
    out[5] = 0
    u16(8, off)
    u16(10, 0)

    def wblk(at: int, b: dict) -> None:
        out[at] = b["op"] & 0xFF
        out[at + 1] = b["flags"] & 0xFF
        u16(at + 2, b["p0"])
        i16(at + 4, b["p1"])
        u16(at + 6, b["p2"])
        u16(at + 8, b["p3"])

    for i, st in enumerate(states):
        so = TASK_HDR_LEN + i * TASK_STATE_LEN
        u16(so, noff[i])
        u16(so + 2, eoff[i])
        u16(so + 4, boff[i])
        out[so + 6] = len(st["enter"])
        out[so + 7] = len(st["body"])
        for j, b in enumerate(st["enter"]):
            wblk(eoff[i] + j * TASK_BLOCK_LEN, b)
        for j, b in enumerate(st["body"]):
            wblk(boff[i] + j * TASK_BLOCK_LEN, b)
        if names[i]:
            out[noff[i]:noff[i] + len(names[i])] = names[i]

    u16(6, pr.crc16_ccitt(bytes(out[TASK_HDR_LEN:])))
    return bytes(out)


@pytest.fixture(scope="module")
def lap_image() -> bytes:
    """`renwu-lap.json`（页面"导入任务文件"认的那份）编出来的镜像。"""
    with open(LAP_JSON, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    assert doc["ver"] == 1 and len(doc["tasks"]) == 1
    prog = doc["tasks"][0]["prog"]
    img = _encode_prog(prog)
    assert len(img) == LAP_LEN, f"长度与 web 侧钉的 {LAP_LEN} 不一致：{len(img)}"
    assert len(prog["states"]) == 15 and prog["main"] == 0
    return img


def test_lap_program_is_accepted_by_the_real_firmware(pipe, lap_image):
    """用户那份 2127 B 的全程任务：真固件必须收下、列得出来、回读一致。"""
    img = lap_image
    crc = pr.crc16_ccitt(img[12:])
    name = "旧全程·12障碍"
    up, puts, commit_seq = _upload(img, name=name)
    frames = pipe(*up, pr.frame_task_list(50), pr.frame_task_get(51, 0), pad=400)

    ack = _acks(frames)
    assert all(ack.get(s) == pr.ST_OK for s in puts), f"有 PUT 片被真固件拒了：{ack}"
    assert ack.get(commit_seq) == pr.ST_OK, \
        f"真固件拒收这条长任务（2127B/15 状态/4096 上限）——COMMIT 状态码 {ack.get(commit_seq)}：{ack}"

    items = [pr.decode_task_list_r(f.payload) for f in frames if f.type == pr.T_TASK_LIST_R]
    assert items and items[0] is not None, f"目录里没列出刚烧的任务：{items}"
    assert (items[0]["id"], items[0]["len"], items[0]["crc"], items[0]["flags"]) == (0, len(img), crc, 0), items[0]
    assert items[0]["name"] == name, f"名字没存住：{items[0]['name']!r}"

    got = bytearray()
    end = None
    for f in frames:
        if f.type != pr.T_TASK_DATA:
            continue
        tid, off, data = pr.decode_task_data(f.payload)
        assert tid == 0
        if not data:
            end = off
            break
        assert off == len(got), f"回读分片顺序不对：off={off} 期望 {len(got)}"
        got += data
    assert end == len(img), f"回读没收尾（end={end}，期望 {len(img)}）"
    assert bytes(got) == img, "2127 B 的长任务回读不一致"
