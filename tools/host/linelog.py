"""循迹环实时记录：边驱动边记录遥测里的**灰度/线位置/丢线**与轮速、修正量。

相比 trace.py 的关键区别：本工具记录 `gray[8]` 与 `trace.pos`（**固件实时算出来的线位置**），
所以能直接看出"车在线上的什么位置、有没有丢线、修正量是不是合理"——
调循迹环只有看这些量才有意义（T_CAL_REPORT 只报标定表，不能当实时用）。

用法：
  python A/_tools/linelog.py --sec 4 --mode trace --a 6800
  python A/_tools/linelog.py --sec 3 --mode 1 --a 7200 --b 7200     # 直给两轮
  python A/_tools/linelog.py --sec 2                                 # 只记录，不驱动
输出：CSV 到 A/_tools/pid_logs/line_*.csv + 终端摘要
列：t_ms,pos,lost,vl,vr,pwm_l,pwm_r,corr,rate_ref,ang_err,g0..g7,vbat,ibat
"""
from __future__ import annotations

import argparse
import csv
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402

MODES = {"off": 0, "wheels": 1, "base": 2, "trace": 3}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--sec", type=float, default=4.0)
    ap.add_argument("--mode", default="trace")
    ap.add_argument("--a", type=int, default=0)
    ap.add_argument("--b", type=int, default=0)
    ap.add_argument("--keepalive", type=float, default=0.08)
    args = ap.parse_args()

    mode = MODES.get(str(args.mode).lower(), 1) if not str(args.mode).isdigit() else int(args.mode)
    outdir = os.path.join(HERE, "pid_logs")
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "line_%s.csv" % time.strftime("%Y%m%d%H%M%S"))

    lk = SerialLink(port=args.port, baud=921600)
    lk.start()
    time.sleep(0.7)
    seq = 7000
    k = 0
    t0 = time.time()
    last_drv = -1e9
    rows = []
    with open(path, "w", newline="", encoding="utf-8") as fh:
        wr = csv.writer(fh)
        wr.writerow(["t_ms", "pos", "lost", "vl", "vr", "pwm_l", "pwm_r", "corr", "rate_ref",
                     "ang_err", "g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7", "vbat", "ibat"])
        while time.time() - t0 < args.sec:
            now = time.time()
            if mode != 0 and (now - last_drv) >= args.keepalive:
                last_drv = now
                seq += 1
                lk.send(pr.frame_drive(seq, mode, int(args.a), int(args.b)))
            k += 1
            for f in (lk.poll(k) or []):
                if f.type != pr.T_TELEM2:
                    continue
                d = pr.decode_telemetry(f.payload)
                g = d.get("gray") or [0] * 8
                tr = d.get("trace") or {}
                an = d.get("angle") or {}
                rows.append([d.get("t", 0), tr.get("pos", 0.0), 1 if d.get("flags", {}).get("lost") else 0,
                             d.get("vl", 0), d.get("vr", 0), d.get("pwm_l", 0), d.get("pwm_r", 0),
                             an.get("corr", 0.0), an.get("rate_ref", 0.0), an.get("err", 0.0)]
                            + list(g)[:8] + [d.get("vbat_mv", 0), d.get("ibat_ma", 0)])
            time.sleep(0.002)
        if mode != 0:
            seq += 1
            lk.send(pr.frame_drive(seq, 0, 0, 0))
        for r in rows:
            wr.writerow(r)
    lk.stop()

    print("saved: %s  (%d frames)" % (os.path.basename(path), len(rows)))
    if not rows:
        print("BAD: no telemetry")
        return 1
    m = [r for r in rows if abs(r[3]) > 20 or abs(r[4]) > 20]
    lost = sum(r[2] for r in rows)
    print("  lost frames: %d/%d (%.1f%%)" % (lost, len(rows), 100.0 * lost / len(rows)))
    print("  pos: med %.2f  min %.2f  max %.2f  (4.5=居中)" %
          (st.median(r[1] for r in rows), min(r[1] for r in rows), max(r[1] for r in rows)))
    dark = [sum(1 for x in r[10:18] if x > 128) for r in rows]
    print("  压线通道数: med %d  max %d  (0=看不到线)" % (st.median(dark), max(dark)))
    if m:
        print("  vl med %.0f  vr med %.0f   速差 med %.0f  p90 %.0f" %
              (st.median(r[3] for r in m), st.median(r[4] for r in m),
               st.median(abs(r[3] - r[4]) for r in m),
               sorted(abs(r[3] - r[4]) for r in m)[int(len(m) * 0.9)]))
        print("  corr(角度环修正) med %.0f  p90 %.0f   rate_ref med %.0f" %
              (st.median(abs(r[7]) for r in m), sorted(abs(r[7]) for r in m)[int(len(m) * 0.9)],
               st.median(abs(r[8]) for r in m)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
