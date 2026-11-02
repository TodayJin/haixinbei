import pymupdf, numpy as np
doc = pymupdf.open("docs/竞赛规则-第七届吉林省AI创新大赛.pdf")
page = doc[48]
clip = pymupdf.Rect(118, 168, 477, 570)
pix = page.get_pixmap(dpi=400, clip=clip)
img = np.frombuffer(pix.samples, dtype=np.uint8).reshape(pix.height, pix.width, pix.n)
gray = img[:, :, :3].mean(axis=2)
dark = gray < 110
print("渲染 %dx%d" % (pix.width, pix.height))

rows = dark.sum(axis=1)
cols = dark.sum(axis=0)
print("\n最长横线（行, 黑像素数）:")
for r in np.argsort(rows)[-8:][::-1]:
    print("   row %4d  n=%d  (%.1f%%)" % (r, rows[r], 100.0 * rows[r] / pix.width))
print("最长竖线（列, 黑像素数）:")
for c in np.argsort(cols)[-8:][::-1]:
    print("   col %4d  n=%d  (%.1f%%)" % (c, cols[c], 100.0 * cols[c] / pix.height))

# ASCII 缩略：把暗像素密度降到 96x76 网格
H, W = 44, 92
gy = np.array_split(np.arange(pix.height), H)
gx = np.array_split(np.arange(pix.width), W)
chars = " .:-=+*#%@"
print("\n=== 跑道缩略图（@ = 黑线密集）===")
for i, ry in enumerate(gy):
    line = ""
    for rx in gx:
        blk = dark[ry[0]:ry[-1] + 1, rx[0]:rx[-1] + 1]
        d = blk.mean()
        line += chars[min(len(chars) - 1, int(d * 22))]
    print("%2d|%s" % (i, line))
