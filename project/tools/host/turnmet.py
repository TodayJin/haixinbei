"""角度环阶跃指标计算（可信版）—— 取代我之前每次临时拼的 inline 脚本（那些出过错）。

用法：
  python A/_tools/turnmet.py                 # 分析 pid_logs 里最近 3 个 turn2_*.csv
  python A/_tools/turnmet.py --last 6        # 最近 6 个
  python A/_tools/turnmet.py --files a.csv b.csv

必守规则（我踩过的坑）：
  · **只从阶跃标记行（首列 -1）之后取数** —— 标记前是上一趟的尾巴，取错会得出"转到一半又回去"的假象
  · 到位时间 = 首次进入 ±5% 且此后不再出带（不是"第一次碰到"）
  · 抖动 = **到位后**最后 0.5s 的峰峰值（这才是"是否还在晃"；用整段末 40% 会把接近过程算进去）
  · 转率用 50ms 窗口算（5ms 窗口会被 yaw 的 0.1° 量化噪声淹没 ⇒ 出现 900°/s 的假值）

输出列：峰值/超调%/到位s/|静差|/末0.5s抖动/峰值转率/(是否达标)
"""
from __future__ import annotations

import argparse
import glob
import os
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "pid_logs")
TGT = 90.0
BAND = 0.05 * TGT


def norm(a: float) -> float:
    while a > 180.0:
        a -= 360.0
    while a <= -180.0:
        a += 360.0
    return a


def analyse(path: str):
    raw0 = [l.strip().split(",") for l in open(path, encoding="utf-8") if not l.startswith("t_ms")]
    mk = next((i for i, r in enumerate(raw0) if r and r[0] == "-1"), None)
    raw = [r for r in raw0 if len(r) >= 9 and r[0] != "-1"]
    if mk is None or mk < 5 or len(raw) - mk < 50:
        return None
    t = [float(r[0]) / 1000.0 for r in raw]
    yaw = [float(r[1]) for r in raw]
    cont = [yaw[0]]
    for i in range(1, len(yaw)):
        cont.append(cont[-1] + norm(yaw[i] - yaw[i - 1]))
    ref = cont[max(0, mk - 1)]
    rel = [c - ref for c in cont]
    idx = list(range(mk, len(raw)))
    ts = [t[i] - t[mk] for i in idx]
    r2 = [rel[i] for i in idx]
    peak = max(r2)
    settle = None
    for j in range(len(r2)):
        if all(abs(v - TGT) <= BAND for v in r2[j:]):
            settle = ts[j]
            break
    tail = [r2[j] for j in range(len(ts)) if ts[j] >= ts[-1] - 0.5]
    jit = (max(tail) - min(tail)) if tail else -1.0
    # 峰值转率（50ms 窗口）
    j0 = 0
    vmax = 0.0
    for i in range(mk, len(t)):
        while t[i] - t[j0] > 0.05:
            j0 += 1
        dt = t[i] - t[j0]
        if dt > 0.01:
            v = (cont[i] - cont[j0]) / dt
            if v > vmax:
                vmax = v
    return dict(peak=peak, over=peak - TGT, settle=settle, stat=abs(r2[-1] - TGT), jit=jit, vmax=vmax)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--last", type=int, default=3)
    ap.add_argument("--files", nargs="*", default=None)
    a = ap.parse_args()
    files = a.files if a.files else sorted(glob.glob(os.path.join(LOGS, "turn2_*.csv")))[-a.last:]
    if not files:
        print("no turn2_*.csv found")
        return 1
    print("%-26s %7s %8s %8s %8s %11s %9s %s" %
          ("file", "peak", "over%", "settle", "|stat|", "jit0.5s", "v_peak", "verdict"))
    nok = 0
    for p in files:
        m = analyse(p)
        if not m:
            print("%-26s  (标记异常/数据不足)" % os.path.basename(p))
            continue
        ok = (m["over"] < 13.5) and (m["jit"] < 3.0) and (m["stat"] < 2.0)
        if ok:
            nok += 1
        print("%-26s %7.1f %+8.1f %8s %8.1f %11.1f %9.0f %s" %
              (os.path.basename(p), m["peak"], m["over"],
               ("%.2f" % m["settle"]) if m["settle"] is not None else "no-band",
               m["stat"], m["jit"], m["vmax"], "OK" if ok else "-"))
    print("=> 达标 %d/%d  (超调<13.5° 且 到位后抖动<3° 且 |静差|<2°)" % (nok, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
