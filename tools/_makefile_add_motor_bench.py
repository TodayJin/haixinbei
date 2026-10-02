#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性：给 firmware/Makefile 加 MOTOR_BENCH=1 构建分支。

为什么要这么写：Makefile 不是干净的单一编码 —— 大部分是 UTF-8，但有几个
汉字位置是坏字节序列（用 UTF-8 解出来会得到 U+FFFD）。所以任何"按某一种
编码整体读写"的做法都会把那几处毁掉。

这里用的是**字节保持**的做法：按 latin-1 解码（每个字节 <-> 一个码点，一一
对应）在字符串上做纯 ASCII 的替换，再按 latin-1 编码写回。非 ASCII 的字节
原样穿过，一个都不会动。

替换都带 count==1 断言：跑第二遍会直接失败，不会把文件改坏。
"""
import sys
from pathlib import Path

MAKEFILE = Path(__file__).resolve().parents[1] / "firmware" / "Makefile"

# 要插进去的中文注释：先编成 UTF-8 字节，再按 latin-1 逐字节接进字节流里
CN_BENCH = "电机 + 编码器台架自测固件".encode("utf-8").decode("latin-1")
CN_NOTE = ("#    台架自测在 Encoder_Init() 之后、周期任务起来之前跑，"
           "结果走 RTT").encode("utf-8").decode("latin-1")

REPLACEMENTS = [
    # 1) 输出目录/目标名：单独的 build-bench，不覆盖正式固件
    (
        "    BUILD  = build-diag\n  endif\nendif",
        "    BUILD  = build-diag\n  endif\n"
        "  ifdef MOTOR_BENCH\n"
        "    TARGET = haixin-bench\n"
        "    BUILD  = build-bench\n"
        "  endif\n"
        "endif",
    ),
    # 2) 编译开关
    (
        "  ifdef DIAG\n    C_DEFS += -DAPP_OLED_DIAG\n  endif\nendif",
        "  ifdef DIAG\n    C_DEFS += -DAPP_OLED_DIAG\n  endif\n"
        "  ifdef MOTOR_BENCH\n"
        "    C_DEFS += -DMOTOR_BENCH\n"
        "  endif\n"
        "endif",
    ),
    # 3) 用法注释
    (
        "#    make flash / make clean / make size\n",
        "#    make flash / make clean / make size\n"
        f"#    make MOTOR_BENCH=1  -> {CN_BENCH}\n"
        f"{CN_NOTE}\n",
    ),
]


def main() -> int:
    raw = MAKEFILE.read_bytes()
    text = raw.decode("latin-1")          # 字节保持

    for old, new in REPLACEMENTS:
        n = text.count(old)
        if n != 1:
            print(f"[FAIL] 锚点出现 {n} 次，预期 1 次。文件没动。")
            print("       锚点: %r" % old[:70])
            return 1
        text = text.replace(old, new)

    out = text.encode("latin-1")
    MAKEFILE.write_bytes(out)
    print(f"[OK] {MAKEFILE}")
    print(f"     {len(raw)} -> {len(out)} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
