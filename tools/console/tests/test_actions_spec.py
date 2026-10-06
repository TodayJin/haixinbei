"""动作表一致性：`firmware/App/Bsp/servo.c` 的 `k_action_default` / `k_action_name` 才是真值。

上位机（`bridge/params.py` 的 `ACTION_*`）和仿真车（`bridge/sim.py` 的 `ACTION_DEFAULT`）
都各自抄了一份，抄错就会出现"页面上一排动作全是脏的"这种假象 —— 2026-10-03 真的踩过：
sim.py 抄的是**台架上改过的运行值**，而不是出厂表，于是 `/api/actions` 里 12/14 项
`dirty=true`，`preset_a1/a2` 全是别人的数。这个测试直接把固件源码解析出来比对。
"""
from __future__ import annotations

import os
import re

from bridge import params as P
from bridge import sim as S

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SERVO_C = os.path.join(REPO, "firmware", "App", "Bsp", "servo.c")
SERVO_H = os.path.join(REPO, "firmware", "App", "Bsp", "servo.h")


def _strip_comments(src: str) -> str:
    """去掉 C 注释（`/* */` 与 `//`）。

    ★这一步是必须的：`k_action_default[]` 表体里有一条注释写着「默认 {0,0}」，
    而下面的正则按 `{数字,数字}` 抓行 ⇒ 不去注释会把这条注释也算成**多出来的一行**
    （实测 21 行 vs 真值 20 行），`test_factory_poses_match_firmware` 与
    `test_firmware_pipe.py::test_action_pose_set_is_range_checked_by_the_real_firmware`
    就一起红在"表比固件多一项"上。
    """
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", src, flags=re.S))


def _firmware_source() -> str:
    """servo.c 的正文（已去注释）。"""
    with open(SERVO_C, "r", encoding="utf-8") as fh:
        return _strip_comments(fh.read())


def _firmware_action_count() -> int:
    """固件真值：`servo.h` 的 `#define SERVO_ACTION_N <n>`。

    动作表长度**只认固件这一处**（`bridge/params.py` 的 `ACTION_COUNT` 也是从名字表
    推出来的，不是写死的）：14 → 20 那次扩容之所以漏了一片，就是因为别处写死了 14。
    """
    with open(SERVO_H, "r", encoding="utf-8") as fh:
        m = re.search(r"#define\s+SERVO_ACTION_N\s+(\d+)", fh.read())
    assert m, "firmware/App/Bsp/servo.h 里找不到 SERVO_ACTION_N"
    return int(m.group(1))


def _block(src: str, marker: str) -> str:
    """取 `marker = {` 到第一个 `};` 之间的正文（数组初始化用）。"""
    head, sep, tail = src.partition(marker)
    assert sep, f"servo.c 里找不到 {marker}"
    body, sep2, _ = tail.partition("};")
    assert sep2, f"{marker} 没有以 }}; 收尾"
    return body


def _from_marker_to_eof(src: str, marker: str) -> str:
    """取 `marker` 之后到文件末尾的正文（函数体里没有 `};`，不能按 `};` 切）。"""
    head, sep, tail = src.partition(marker)
    assert sep, f"servo.c 里找不到 {marker}"
    return tail


def _firmware_defaults() -> list[tuple[int, int]]:
    body = _block(_firmware_source(), "k_action_default[SERVO_ACTION_N][2] = {")
    return [(int(a), int(b)) for a, b in
            re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", body)]


def _firmware_names() -> list[str]:
    body = _block(_firmware_source(), "k_action_name[SERVO_ACTION_N] = {")
    return re.findall(r'"([^"]*)"', body)


def test_firmware_tables_parse():
    """两张表都要解析出 `SERVO_ACTION_N` 项（行数从 servo.h 取，不再写死）。"""
    n = _firmware_action_count()
    fw = _firmware_defaults()
    names = _firmware_names()
    assert len(fw) == n, f"k_action_default 解析到 {len(fw)} 行，servo.h 的 SERVO_ACTION_N = {n}"
    assert len(names) == n, f"k_action_name 解析到 {len(names)} 项，servo.h 的 SERVO_ACTION_N = {n}"
    # 三份镜像（桥 / 仿真车 / 这里的固件解析结果）必须同长
    assert len(P.ACTION_FACTORY) == n, f"params.ACTION_FACTORY 有 {len(P.ACTION_FACTORY)} 项"
    assert len(S.ACTION_NAMES) == n and len(S.ACTION_DEFAULT) == n, \
        f"sim 的动作表有 {len(S.ACTION_NAMES)} 项"


def test_tail_slots_are_neutral_placeholders():
    """扩表补的尾部槽位（现在 = 15..20）：名字是自动生成的「动作NN」、默认角度 0/0。

    判据**不写死 15/20**（下次再扩表不用改这里）：凡是名字等于「动作NN」的槽位就是
    自动占位槽 —— 它们必须 ① 自己的第 N 项默认角度是 0/0；② 连续地待在表尾
    （也就是老的 14 项一个都没被顶掉/插队）。镜像表与固件逐项相等由上面两条测试盯着。
    """
    names, poses = _firmware_names(), _firmware_defaults()
    here = [i for i, nm in enumerate(names) if nm == f"动作{i + 1}"]
    assert here, "一个占位槽都没有：扩表时补的「动作15..20」不见了？"
    assert here[0] > 0, "第 1 个动作不该是占位槽"
    assert all(poses[i] == (0, 0) for i in here), \
        f"占位槽的默认角度必须是 0/0：{[(i + 1, poses[i]) for i in here]}"
    assert here == list(range(here[0], len(names))), \
        f"占位槽必须连续排在表尾：{[(i + 1, names[i]) for i in here]}（全表 {len(names)} 项）"


def test_names_match_firmware():
    fw = _firmware_names()
    assert P.ACTION_NAMES == fw, f"params.ACTION_NAMES 与固件不一致：{P.ACTION_NAMES} vs {fw}"
    assert S.ACTION_NAMES == fw, f"sim.ACTION_NAMES 与固件不一致：{S.ACTION_NAMES} vs {fw}"
    assert P.ACTION_COUNT == len(fw)


def test_factory_poses_match_firmware():
    fw = _firmware_defaults()
    mine = [tuple(a) for a in P.ACTION_FACTORY]
    theirs = [tuple(a) for a in S.ACTION_DEFAULT]
    assert mine == fw, f"params.ACTION_FACTORY 抄错了：{mine} vs 固件 {fw}"
    assert theirs == fw, f"sim.ACTION_DEFAULT 抄错了：{theirs} vs 固件 {fw}"


def test_compound_actions_are_the_ones_with_middle_steps():
    """固件 Servo_Move() 里只有 5/9 号带 HAL_Delay 中间步骤。"""
    body = _from_marker_to_eof(_firmware_source(), "void Servo_Move(int x)")
    idx = P.ACTION_COMPOUND.keys()
    assert set(idx) == {5, 9}, f"上位机标了这些复合动作：{sorted(idx)}"
    assert set(S.COMPOUND) == {5, 9}
    for n in idx:
        assert re.search(rf"x\s*==\s*{n}\b", body), f"固件 Servo_Move 里没有 {n} 号的复合分支"


def test_action_items_shape_and_dirty_rule():
    vals = [list(a) for a in P.ACTION_FACTORY]
    items = P.action_items(vals)
    # 项数不写死：出厂表多长就该有几项，而且必须等于 servo.h 的 SERVO_ACTION_N
    assert len(items) == len(vals) == _firmware_action_count()
    assert [it["n"] for it in items] == list(range(1, len(vals) + 1))
    assert [it["name"] for it in items] == list(P.ACTION_NAMES)
    for it in items:
        assert set(it) >= {"n", "name", "a1", "a2", "dirty", "compound", "steps"}
        assert it["dirty"] is False, "出厂值不该是脏的"

    vals[2] = [10, 20]                       # 车上第 3 个动作被改过
    it = P.action_items(vals)[2]
    assert it["n"] == 3 and (it["a1"], it["a2"]) == (10, 20)
    assert it["dirty"] is True
    assert (it["preset_a1"], it["preset_a2"]) == tuple(P.ACTION_FACTORY[2])

    vals[2] = [999, -5]                      # 越界值（如 Flash 损坏）⇒ 夹到 0..180
    it = P.action_items(vals)[2]
    assert (it["a1"], it["a2"]) == (180, 0)
    assert it["dirty"] is True

    vals[2] = P.ACTION_FACTORY[2]            # 本机预设优先于出厂值参与比较
    preset = [list(a) for a in P.ACTION_FACTORY]
    preset[2] = [10, 20]
    it = P.action_items(vals, preset=preset)[2]
    assert (it["preset_a1"], it["preset_a2"]) == (10, 20)
    assert it["dirty"] is True
