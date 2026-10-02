"""从 GME12864-41.pdf 提引脚表 / 初始化代码 / 参考电路页。"""
import fitz

from _paths import ds

doc = fitz.open(ds("GME12864-41.pdf"))
for pg in (30, 31, 32, 33):
    t = doc[pg - 1].get_text()
    print("##### PDF p%d (len %d) #####" % (pg, len(t)))
    print(t[:2600].encode("ascii", "replace").decode())
    print()
