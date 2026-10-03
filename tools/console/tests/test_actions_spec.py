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
    src = open(SERVO_C, "r", encoding="utf-8").read()
    body = _block(src, "k_action_default[SERVO_ACTION_N][2] = {")
    return [(int(a), int(b)) for a, b in
            re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", body)]


def _firmware_names() -> list[str]:
    src = open(SERVO_C, "r", encoding="utf-8").read()
    body = _block(src, "k_action_name[SERVO_ACTION_N] = {")
    return re.findall(r'"([^"]*)"', body)


def test_firmware_tables_parse():
    assert len(_firmware_defaults()) == 14, "k_action_default 解析到的行数不对"
    assert len(_firmware_names()) == 14, "k_action_name 解析到的项数不对"


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
    src = open(SERVO_C, "r", encoding="utf-8").read()
    body = _from_marker_to_eof(src, "void Servo_Move(int x)")
    idx = P.ACTION_COMPOUND.keys()
    assert set(idx) == {5, 9}, f"上位机标了这些复合动作：{sorted(idx)}"
    assert set(S.COMPOUND) == {5, 9}
    for n in idx:
        assert re.search(rf"x\s*==\s*{n}\b", body), f"固件 Servo_Move 里没有 {n} 号的复合分支"


def test_action_items_shape_and_dirty_rule():
    vals = [list(a) for a in P.ACTION_FACTORY]
    items = P.action_items(vals)
    assert len(items) == 14
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
