import pymupdf
doc = pymupdf.open("docs/竞赛规则-第七届吉林省AI创新大赛.pdf")
spage = doc[48]
FL, FT, FR, FB = 162.8, 195.4, 414.6, 437.6
W, H = FR - FL, FB - FT
DPI = 500
sc = DPI / 72.0
ox, oy = FL - 6, FT - 6

def rect_cm(cx_cm, cy_cm, w_cm, h_cm, pad=12.0):
    x0 = FL + W * (cx_cm - w_cm / 2 - pad) / 450.0
    x1 = FL + W * (cx_cm + w_cm / 2 + pad) / 450.0
    y0 = FT + H * (cy_cm - h_cm / 2 - pad) / 450.0
    y1 = FT + H * (cy_cm + h_cm / 2 + pad) / 450.0
    return pymupdf.Rect(x0, y0, x1, y1) & spage.rect

# cm 中心, 该标号在图上占的 cm 尺寸（用于定裁剪范围，给足余量）
items = [(112, 158, 14, 8), (147, 214, 8, 22), (101, 260, 16, 8),
         (253, 117, 16, 8), (254, 76, 20, 14), (359, 218, 9, 26),
         (389, 216, 9, 24), (252, 325, 26, 9), (148, 375, 8, 28),
         (297, 381, 8, 25)]
cells = [(("%.0f,%.0f" % (c[0], c[1])), rect_cm(*c)) for c in items]
cells = [(t, r) for t, r in cells if not r.is_empty and r.width > 1 and r.height > 1]

cols, cw, ch, lab = 2, 300, 170, 15
rows = (len(cells) + cols - 1) // cols
out = pymupdf.open()
pg = out.new_page(width=cols * cw, height=rows * (ch + lab))
for i, (tag, r) in enumerate(cells):
    cx = (i % cols) * cw
    cy = (i // cols) * (ch + lab)
    pg.insert_text((cx + 6, cy + 11), "cm(" + tag + ")", fontsize=10,
                   fontname="china-s", color=(0.85, 0, 0))
    pg.draw_rect(pymupdf.Rect(cx + 3, cy + lab, cx + cw - 3, cy + lab + ch),
                 color=(0.7, 0.7, 0.7), width=0.4)
    pg.show_pdf_page(pymupdf.Rect(cx + 4, cy + lab + 1, cx + cw - 4, cy + lab + ch - 1),
                     doc, 48, clip=r)
pix = pg.get_pixmap(dpi=300)
pix.save("out/rules/map/labels.png")
print("labels.png", pix.width, "x", pix.height, "cells:", len(cells))
