"""把 GME12864-41.pdf 的参考电路页渲染成 PNG 到 out/，用来逐脚核对 OLED 接法。

用法:
    python tools/render_gme.py            # 默认渲染第 31 页（0-based 30）
    python tools/render_gme.py 30 31 32   # 指定 1-based 页码
"""
import sys

import fitz

from _paths import ds, out

doc = fitz.open(ds("GME12864-41.pdf"))
pages = [int(a) for a in sys.argv[1:]] or [31]
for pg in pages:
    p = doc[pg - 1]
    r = p.rect
    # 参考电路在图幅左侧偏中，裁掉页边空白
    clip = fitz.Rect(r.x0 + 90, r.y0 + 60, r.x0 + 330, r.y0 + 330)
    pix = p.get_pixmap(matrix=fitz.Matrix(9, 9), clip=clip)
    path = out(f"gme_p{pg}.png")
    pix.save(path)
    print("saved", path, pix.width, pix.height)
