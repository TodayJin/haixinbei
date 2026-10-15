"""看 CSV 里 host_drive / motor 的时间线：会话什么时候掉、掉多久、有没有周期性。"""
import glob
import os
import sys

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pid_logs")
fs = sorted(glob.glob(os.path.join(LOGS, "trace_*.csv")))
if not fs:
    print("no csv")
    sys.exit(1)
p = fs[-1]
print("file:", os.path.basename(p))

rows = []
with open(p, encoding="utf-8") as fh:
    for line in fh:
        if line.startswith("#") or line.startswith("t_ms"):
            continue
        v = line.strip().split(",")
        if len(v) >= 11:
            rows.append([int(float(x)) for x in v[:11]])
if not rows:
    print("no rows with 11 fields")
    sys.exit(1)

t = [r[0] for r in rows]
hdr = [r[9] for r in rows]
mot = [r[8] for r in rows]

# 会话"掉"的区间（hdr 从 1 变 0 到再变 1）
print("\n=== host_drive 时间线 ===")
print("  hdr==1 帧数 %d/%d (%.0f%%)" % (sum(hdr), len(hdr), 100.0 * sum(hdr) / len(hdr)))
segs = []
i = 0
while i < len(rows):
    if hdr[i] == 0:
        j = i
        while j + 1 < len(rows) and hdr[j + 1] == 0:
            j += 1
        segs.append((t[i], t[j] - t[i] + 1, i, j))
        i = j + 1
    else:
        i += 1
print("  会话为 0 的连续区间共 %d 段" % len(segs))
for (t0, dur, i0, j0) in segs[:12]:
    print("     t=%6d ms  持续 %4d ms  帧数 %3d" % (t0, dur, j0 - i0 + 1))
if segs:
    ds = [s[1] for s in segs]
    print("  区间时长: min %d  max %d  median %d ms" % (min(ds), max(ds), sorted(ds)[len(ds)//2]))
    if len(segs) > 2:
        gaps = [segs[k+1][0] - segs[k][0] for k in range(len(segs)-1)]
        print("  相邻区间起点间隔: " + ", ".join(str(g) for g in gaps[:12]) + " ms")
        print("  => 若间隔≈500ms 就是看门狗；若间隔≈80~200ms 就是每次重发都被清 ✓")
print("\n=== 头 40 帧 (t, hdr, mot, pwm_l, vl) ===")
for r in rows[:40]:
    print("   t=%5d hdr=%d mot=%d pwm=%6d vl=%5d" % (r[0], r[9], r[8], r[3], r[1]))
