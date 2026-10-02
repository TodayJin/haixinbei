import pymupdf
src = pymupdf.open("docs/竞赛规则-第七届吉林省AI创新大赛.pdf")
spage = src[48]
out = pymupdf.open()
pg = out.new_page(width=spage.rect.width, height=spage.rect.height)
pg.show_pdf_page(pg.rect, src, 48)

FL, FT, FR, FB = 162.8, 195.4, 414.6, 437.6
W, H = FR - FL, FB - FT
def X(cm): return FL + W * cm / 450.0
def Y(cm): return FT + H * cm / 450.0

pg.draw_rect(pymupdf.Rect(FL, FT, FR, FB), color=(1, 0, 0), width=0.8)
for i in range(1, 10):
    c = i * 45.0
    pg.draw_line(pymupdf.Point(X(c), FT), pymupdf.Point(X(c), FB), color=(0, 0.75, 0.9), width=0.35)
    pg.draw_line(pymupdf.Point(FL, Y(c)), pymupdf.Point(FR, Y(c)), color=(0, 0.75, 0.9), width=0.35)
for i in range(1, 10):
    c = i * 45.0
    pg.insert_text((X(c) - 5, FT - 3), str(int(c)), fontsize=5, fontname="helv", color=(0, 0, 0.85))
    pg.insert_text((FL - 13, Y(c) + 2), str(int(c)), fontsize=5, fontname="helv", color=(0, 0, 0.85))
pix = pg.get_pixmap(dpi=340, clip=pymupdf.Rect(140, 175, 430, 462))
pix.save("out/rules/map/grid.png")
print("ok", pix.width, "x", pix.height)
