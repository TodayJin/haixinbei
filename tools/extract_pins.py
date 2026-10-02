"""从 stm32g474re.pdf 提 PA0-PA7 的引脚/复用/电平（DS12288 pinout 表）。"""
import fitz

from _paths import ds

doc = fitz.open(ds("stm32g474re.pdf"))
targets = {"PA0", "PA1", "PA2", "PA3", "PA4", "PA5", "PA6", "PA7"}
for i in range(56, 64):
    lines = [l.strip() for l in doc[i].get_text().split("\n")]
    for k, l in enumerate(lines):
        if l in targets:
            ctx = " | ".join(lines[k:k + 4])
            print(f"p{i + 1}: {ctx[:120]}")
