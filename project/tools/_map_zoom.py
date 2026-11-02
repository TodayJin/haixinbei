import pymupdf, os
os.makedirs("out/rules/map", exist_ok=True)
doc = pymupdf.open("docs/竞赛规则-第七届吉林省AI创新大赛.pdf")
page = doc[48]
# 地图图块 bbox: (123,173)-(472,564)
X0, Y0, X1, Y1 = 118, 168, 477, 570
mx, my = (X1 - X0) * 0.04, (Y1 - Y0) * 0.04
cx, cy = (X0 + X1) / 2, (Y0 + Y1) / 2
quads = {"NW": (X0 - mx, Y0 - my, cx + mx, cy + my),
         "NE": (cx - mx, Y0 - my, X1 + mx, cy + my),
         "SW": (X0 - mx, cy - my, cx + mx, Y1 + my),
         "SE": (cx - mx, cy - my, X1 + mx, Y1 + my)}
for name, r in quads.items():
    pix = page.get_pixmap(dpi=560, clip=pymupdf.Rect(*r))
    path = "out/rules/map/%s.png" % name
    pix.save(path)
    print(name, pix.width, "x", pix.height, "->", path)
