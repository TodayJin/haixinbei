import pymupdf, numpy as np
from collections import deque

doc = pymupdf.open("docs/竞赛规则-第七届吉林省AI创新大赛.pdf")
page = doc[48]
FL, FT, FR, FB = 162.8, 195.4, 414.6, 437.6
W, H = FR - FL, FB - FT
DPI = 500
sc = DPI / 72.0
clip = pymupdf.Rect(FL - 6, FT - 6, FR + 6, FB + 6)
pix = page.get_pixmap(dpi=DPI, clip=clip)
img = np.frombuffer(pix.samples, dtype=np.uint8).reshape(pix.height, pix.width, pix.n)[:, :, :3].astype(int)
R, G, B = img[:, :, 0], img[:, :, 1], img[:, :, 2]
# 障碍标号是较深的蓝字；出发区是浅蓝底（R≈G≈200+）
mask = (B - R > 55) & (B > 110) & (G < B - 25) & (R < 160)
print("蓝色像素:", int(mask.sum()))

h, w = mask.shape
lab = np.zeros((h, w), np.int32)
boxes = []
for y0 in range(h):
    for x0 in range(w):
        if mask[y0, x0] and lab[y0, x0] == 0:
            q = deque([(y0, x0)]); lab[y0, x0] = 1
            ys = [y0]; xs = [x0]
            while q:
                y, x = q.popleft()
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        ny, nx = y + dy, x + dx
                        if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and lab[ny, nx] == 0:
                            lab[ny, nx] = 1; q.append((ny, nx)); ys.append(ny); xs.append(nx)
            boxes.append((min(xs), min(ys), max(xs), max(ys), len(xs)))

# 合并邻近的小块（一个字一个连通域）
def merge(boxes, gap=28):
    out = []
    for b in sorted(boxes, key=lambda t: (t[1], t[0])):
        for i, o in enumerate(out):
            if not (b[0] > o[2] + gap or b[2] < o[0] - gap or b[1] > o[3] + gap or b[3] < o[1] - gap):
                out[i] = (min(o[0], b[0]), min(o[1], b[1]), max(o[2], b[2]), max(o[3], b[3]), o[4] + b[4])
                break
        else:
            out.append(b)
    return out

for _ in range(3):
    boxes = merge(boxes)
groups = [b for b in boxes if b[4] > 120]
print("标号块数:", len(groups))
print("%-8s %-8s %-7s %s" % ("x(cm)", "y(cm)", "像素", "框(w x h)"))
for x0, y0, x1, y1, n in sorted(groups, key=lambda b: b[4], reverse=True):
    cx = ((x0 + x1) / 2 / sc + (FL - 6) - FL) / W * 450
    cy = ((y0 + y1) / 2 / sc + (FT - 6) - FT) / H * 450
    print("%-8.0f %-8.0f %-7d %dx%d" % (cx, cy, n, (x1 - x0), (y1 - y0)))
