"""从 SSD1315.pdf 提命令表 / 电荷泵 / 对比度（IREF）相关页。"""
import re

import fitz

from _paths import ds

doc = fitz.open(ds("SSD1315.pdf"))
for i in range(23, 36):
    t = doc[i].get_text()
    if re.search(r"Command Table|Command Description|81h|8Dh|ADh|Charge Pump", t):
        print("##### p%d #####" % (i + 1))
        print(t[:2600].encode("ascii", "replace").decode())
        print()
