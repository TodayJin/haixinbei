"""核对固件参数表与上位机参数表是否逐项一致（含 HELLO 帧用的 param_crc）。

为什么要这个脚本：HELLO 帧里的 `param_crc` 让上位机发现「两边参数表漂移」，而默认值
必须两边同时改。2026-09-30 增益 ×7.2 那次，原本的手工核对脚本（out/_param_crc_check.py
里手抄的一份 FW 列表）忘了同步，于是脚本报 MISMATCH 而固件其实是对的 —— 手工抄写的
核对表本身就成了漂移源。这里改成**直接解析固件源码**：host_link.h 的 id 宏 +
host_link.c 的 SPECS 默认值，再与 protocol.PARAMS 比。

用法：
    python tools/host/check_param_table.py        # 一致返回 0，不一致返回 1
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import protocol as P  # noqa: E402

HEADER = ROOT / "firmware" / "App" / "Svc" / "host_link.h"
SOURCE = ROOT / "firmware" / "App" / "Svc" / "host_link.c"

_RE_DEFINE = re.compile(
    r"^#define\s+([A-Za-z_]\w*)\s+"
    r"(-?(?:0[xX][0-9A-Fa-f]+|[0-9]*\.?[0-9]+))[uUlLfF]{0,3}\s*(?:/\*.*)?$")
_RE_SPEC = re.compile(r"\{\s*(HP_[A-Z0-9_]+)\s*,\s*([A-Za-z_]\w*|[0-9.]+)f?\s*,")


def crc16_ccitt(data: bytes) -> int:
    """CRC16/CCITT-FALSE，与 protocol.crc16_ccitt / 固件的 host_link_crc16 同口径。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _num(token: str) -> float:
    return float(int(token, 16)) if token.lower().startswith("0x") else float(token)


def firmware_specs(header: Path = HEADER, source: Path = SOURCE) -> list[tuple[int, float]]:
    """从 host_link.h 的 id 宏 + host_link.c 的 SPECS[] 解析 [(id, 默认值), ...]（保持源码顺序）。"""
    ids: dict[str, int] = {}
    for line in header.read_text(encoding="utf-8").splitlines():
        m = _RE_DEFINE.match(line.strip())
        if m and m.group(1).startswith("HP_"):
            ids[m.group(1)] = int(_num(m.group(2)))
    # SPECS 里有两个默认值写成宏（SPEED_FF_SLOPE / SPEED_FF_FRIC）
    macros: dict[str, float] = {}
    for line in header.read_text(encoding="utf-8").splitlines():
        m = _RE_DEFINE.match(line.strip())
        if m and m.group(1) in ("SPEED_FF_SLOPE", "SPEED_FF_FRIC"):
            macros[m.group(1)] = _num(m.group(2))

    specs: list[tuple[int, float]] = []
    for m in _RE_SPEC.finditer(source.read_text(encoding="utf-8")):
        name, default = m.group(1), m.group(2)
        if name not in ids:
            raise SystemExit("SPECS 里的 %s 在 host_link.h 里没有 id 宏" % name)
        value = macros[default] if default in macros else _num(default)
        specs.append((ids[name], value))
    return specs


def host_specs() -> list[tuple[int, float]]:
    return [(p.pid, p.default) for p in P.PARAMS]


def table_crc(pairs: list[tuple[int, float]]) -> int:
    return crc16_ccitt(b"".join(struct.pack("<Hf", i, v) for i, v in pairs))


def compare() -> tuple[list[tuple[int, float]], list[tuple[int, float]], list[str]]:
    fw, host = firmware_specs(), host_specs()
    problems: list[str] = []
    if len(fw) != len(host):
        problems.append("项数不同：固件 %d 项 vs 上位机 %d 项" % (len(fw), len(host)))
    names = {p.pid: p.key for p in P.PARAMS}
    for a, b in zip(fw, host):
        if a != b:
            problems.append("0x%04X %s：固件 %g vs 上位机 %g"
                            % (a[0], names.get(a[0], "?"), a[1], b[1]))
    if table_crc(fw) != P.param_table_crc():
        problems.append("param_crc 不同：固件算 0x%04X vs protocol.param_table_crc() 0x%04X"
                        % (table_crc(fw), P.param_table_crc()))
    return fw, host, problems


def main() -> int:
    fw, host, problems = compare()
    print("固件 host_link.c SPECS : n=%d crc=0x%04X" % (len(fw), table_crc(fw)))
    print("上位机 protocol.PARAMS : n=%d crc=0x%04X" % (len(host), P.param_table_crc()))
    if problems:
        for p in problems:
            print("!! " + p)
        return 1
    print("OK 两张表逐项相同（id / 默认值 / 顺序 / param_crc）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
