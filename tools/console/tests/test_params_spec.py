"""参数表一致性：`bridge/params.py` 是给上位机用的元数据，固件 `host_link.c` 的 SPECS[] 才是真值。

这个测试直接解析固件源码，断言两边的 **id 集合、默认值、范围** 完全一致 —— 防的是一边
改了另一边忘了改（历史上 `NSPEC` 就漂过）。
"""
from __future__ import annotations

import os
import re

from bridge import params as P

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
HOST_LINK_C = os.path.join(REPO, "firmware", "App", "Svc", "host_link.c")
HOST_LINK_H = os.path.join(REPO, "firmware", "App", "Svc", "host_link.h")


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def _hp_defines() -> dict[str, int]:
    src = _read(HOST_LINK_H)
    return {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"#define\s+(HP_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)", src)}


def _numeric_defines() -> dict[str, float]:
    """host_link.h 里的数值宏（SPECS[] 的默认值可以是宏，例如 SPEED_FF_SLOPE）。"""
    src = _read(HOST_LINK_H)
    out = {}
    for m in re.finditer(r"#define\s+([A-Z][A-Z0-9_]*)\s+([-+]?[0-9]*\.?[0-9]+)[fF]?\b", src):
        try:
            out[m.group(1)] = float(m.group(2))
        except ValueError:
            pass
    return out


def _specs() -> list[tuple[str, float, float, float]]:
    src = _read(HOST_LINK_C)
    block = src.split("SPECS[] = {", 1)[1].split("\n};", 1)[0]
    defines = _numeric_defines()

    def num(tok: str) -> float:
        tok = tok.strip()
        if re.fullmatch(r"[-+]?[0-9]*\.?[0-9]+[fF]?", tok):
            return float(tok.rstrip("fF"))
        if tok in defines:
            return defines[tok]
        raise AssertionError(f"SPECS[] 里有个认不出的数：{tok}")

    num_pat = r"([-+0-9.eEfF]+|[A-Z][A-Z0-9_]*)"
    rows = []
    for m in re.finditer(r"\{\s*(HP_[A-Z0-9_]+)\s*,\s*" + num_pat + r"\s*,\s*"
                         + num_pat + r"\s*,\s*" + num_pat + r"\s*\}", block):
        rows.append((m.group(1), num(m.group(2)), num(m.group(3)), num(m.group(4))))
    return rows


def test_every_spec_is_in_the_bridge_table():
    defines = _hp_defines()
    rows = _specs()
    assert len(rows) >= 40, f"只解析到 {len(rows)} 项，SPECS[] 格式变了？"
    for name, default, lo, hi in rows:
        assert name in defines, f"SPECS 里的 {name} 没有对应 #define"
        pid = defines[name]
        spec = P.BY_ID.get(pid)
        assert spec is not None, f"固件有 {name}(0x{pid:04X})，params.py 里没有"
        want_key = name[len("HP_"):].lower()
        assert spec.key == want_key, f"0x{pid:04X} 的 key 不一致：{spec.key} vs {want_key}"
        assert abs(spec.default - default) < 1e-6, f"{name} 默认值：{spec.default} vs {default}"
        assert abs(spec.min - lo) < 1e-6, f"{name} 下限：{spec.min} vs {lo}"
        assert abs(spec.max - hi) < 1e-6, f"{name} 上限：{spec.max} vs {hi}"


def test_bridge_table_has_no_extra_entries():
    defines = _hp_defines()
    specs = _specs()
    declared = {name for name, *_ in specs}
    for spec in P.PARAMS:
        assert "HP_" + spec.key.upper() in declared, \
            f"params.py 里的 {spec.key} 固件 SPECS[] 里没有"
        assert "HP_" + spec.key.upper() in defines
    assert len(P.PARAMS) == len(specs), f"{len(P.PARAMS)} vs {len(specs)}"


def test_param_metadata_is_complete_and_sane():
    groups = set(P.GROUPS)
    keys = set()
    for p in P.PARAMS:
        assert p.key not in keys, f"key 重复：{p.key}"
        keys.add(p.key)
        assert p.id not in (0, 0xFFFF)
        assert p.label.strip()
        assert p.group in groups, f"{p.key} 的分组 {p.group} 不在契约允许的集合里"
        assert p.min <= p.default <= p.max, f"{p.key} 默认值不在范围里"
        assert p.decimals >= 0 and p.step > 0
        assert isinstance(p.unit, str)
    assert len(P.BY_ID) == len(P.PARAMS) == len(P.BY_KEY)
    assert set(P.DEFAULTS) == {p.id for p in P.PARAMS}
    item = P.BY_KEY["pivot_kp"].item(1200.0, dirty=True)
    for field in ("id", "key", "label", "group", "unit", "value", "default", "min",
                  "max", "decimals", "step", "dirty", "note"):
        assert field in item, field
    assert item["value"] == 1200.0 and item["dirty"] is True


def test_lookup_helpers():
    spec = P.BY_KEY["pivot_kp"]
    assert spec.id == 0x0207 and spec.default == 900.0
    assert P.by_id_or_key("pivot_kp") is spec
    assert P.by_id_or_key("0x0207") is spec
    assert P.by_id_or_key(0x0207) is spec
    assert P.by_id_or_key("不存在的键") is None
    assert P.by_id_or_key(True) is None            # bool 不是 id
    assert P.by_id_or_key(None) is None
    assert P.clamp(spec, 1200.0) == 1200.0
    import pytest
    with pytest.raises(ValueError):
        P.clamp(spec, 99999.0)
    with pytest.raises(ValueError):
        P.clamp(spec, float("nan"))

