import pymupdf
doc = pymupdf.open("docs/竞赛规则-第七届吉林省AI创新大赛.pdf")
FL, FT, FR, FB = 162.8, 195.4, 414.6, 437.6
W, H = FR - FL, FB - FT
DPI = 500
sc = DPI / 72.0
ox, oy = FL - 6, FT - 6

# (px_x0, px_y0, px_x1, px_y1) 由上一步检测得到，这里是换算后的 cm 坐标 + 像素框
items = [
    ("112,158", 56, 20, 480, 190),
    ("147,214", 20, 83, 1180, 560),
    ("101,260", 71, 20, 1380, 1160),
    ("253,117", 66, 19, 2000, 640),
    ("254,76",  83, 51, 2000, 460),
    ("359,218", 25, 109, 3040, 1180),
    ("389,216", 25, 98, 3280, 1170),
    ("252,325", 119, 25, 1960, 1900),
    ("148,375", 20, 119, 1100, 2260),
    ("297,381", 20, 103, 2130, 2310),
]
pad_px = 70
cells = []
for label, bw, bh, cx, cy in items:
    x0 = (cx - bw / 2 - pad_px) / sc + ox
    y0 = (cy - bh / 2 - pad_px) / sc + oy
    x1 = (cx + bw / 2 + pad_px) / sc + ox
    y1 = (cy + bh / 2 + pad_px) / sc + oy
    cells.append((label, pymupdf.Rect(x0, y0, x1, y1)))

cols, cw, ch, lab_h = 2, 330, 150, 16
rows = (len(cells) + cols - 1) // cols
src = pymupdf.open("docs/竞赛规则-第七届吉林省AI创新大赛.pdf")
out = pymupdf.open()
pg = out.new_page(width=cols * cw, height=rows * (ch + lab_h))
for i, (label, r) in enumerate(cells):
    cx = (i % cols) * cw
    cy = (i // cols) * (ch + lab_h)
    pg.insert_text((cx + 6, cy + 12), "cm(%.0f, %.0f)" % tuple(float(v) for v in label.split(",")),
                   fontsize=11, fontname="china-s", color=(0.8, 0, 0))
    pg.show_pdf_page(pymupdf.Rect(cx + 4, cy + lab_h, cx + cw - 4, cy + lab_h + ch),
                     src, 48, clip=r)
pix = pg.get_pixmap(dpi=300)
pix.save("out/rules/map/labels.png")
print("labels.png", pix.width, "x", pix.height, "cells:", len(cells))
