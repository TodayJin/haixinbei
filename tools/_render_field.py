import sys
sys.path.insert(0, 'tools/host')
import pymupdf, field as F

sc = 2.4
W = H = F.FIELD_W
doc = pymupdf.open()
pg = doc.new_page(width=W * sc + 40, height=H * sc + 40)
def X(x): return 20 + x * sc
def Y(y): return 20 + y * sc
pg.draw_rect(pymupdf.Rect(X(0), Y(0), X(450), Y(450)), color=(0.2, 0.25, 0.3), width=1.2)
# 出发区
pg.draw_polyline([pymupdf.Point(X(x), Y(y)) for x, y in F.START_ZONE], color=(0.1, 0.3, 0.55), fill=(0.11, 0.21, 0.31), width=1)
for zx0, zy0, zx1, zy1 in (F.TEST_ZONE_A, F.TEST_ZONE_B):
    pg.draw_rect(pymupdf.Rect(X(zx0), Y(zy0), X(zx1), Y(zy1)), color=(0.1, 0.3, 0.55), fill=(0.09, 0.16, 0.24), width=1)
# 跑道
for (ax, ay), (bx, by) in F.TRACK_SEGMENTS:
    pg.draw_line(pymupdf.Point(X(ax), Y(ay)), pymupdf.Point(X(bx), Y(by)), color=(0.85, 0.86, 0.88), width=F.LINE_W * sc)
acx, acy, ar = F.PLATFORM_ARCH
import math as _m
arch = [pymupdf.Point(X(acx + ar * _m.cos(_m.radians(a))),
                       Y(acy + ar * _m.sin(_m.radians(a))))
        for a in range(180, 361, 10)]
pg.draw_polyline(arch, color=(0.85, 0.86, 0.88), width=F.LINE_W * sc)
for mx, my, mc in F.TRACK_MARKS:
    r = 5 * sc
    pg.draw_rect(pymupdf.Rect(X(mx) - r, Y(my) - r, X(mx) + r, Y(my) + r), color=None, fill=(0.6, 0.1, 0.3), width=0)
# 障碍
for ob in F.OBSTACLES:
    x0, y0, x1, y1 = ob.box
    pg.draw_rect(pymupdf.Rect(X(x0), Y(y0), X(x1), Y(y1)), color=(0.9, 0.6, 0.2), fill=(0.14, 0.13, 0.1), width=1.2)
    pg.insert_text((X(ob.x) - 5, Y(ob.y) + 3), str(ob.oid), fontsize=8, fontname="helv", color=(1, 0.8, 0.3))
    pg.insert_text((X(x0), Y(y0) - 3), ob.name, fontsize=6, fontname="china-s", color=(0.6, 0.65, 0.72))
# 路线
pg.draw_polyline([pymupdf.Point(X(x), Y(y)) for x, y in F.ROUTE], color=(0.3, 0.9, 0.5), width=0.8, dashes="[2 2] 0")
pix = pg.get_pixmap(dpi=110)
pix.save("out/rules/field_model.png")
print("field_model.png", pix.width, "x", pix.height)
