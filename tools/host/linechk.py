"""循迹环静态检查：读 8 路灰度的原始/标定值，算出加权重心 data、误差 err、期望转率 rate_cmd。

不用赛道：把车横跨白/黑边界（或指哪一路挡住），看：
  · 8 路原始值是否**逐路变化**（哪一路压黑线，那一路的值应该明显不同）
  · data（加权重心，1..8，中间 4.5）与 err 是否符合预期方向
  · rate_cmd = −(per_unit·err + pos_kd·ė)，>0 = 向左
用法： python A/_tools/linechk.py [--sec 1.5] [--kp 2700] [--kd 20]
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402


def u16s(b, off, n):
    return [struct.unpack_from("<H", b, off + 2 * i)[0] for i in range(n)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--sec", type=float, default=1.5)
    ap.add_argument("--kp", type=float, default=2700.0)
    ap.add_argument("--kd", type=float, default=20.0)
    a = ap.parse_args()

    lk = SerialLink(port=a.port, baud=921600)
    lk.start()
    time.sleep(0.7)
    seq, k = 4000, 0
    reports = []
    telem = 0
    lk.send(pr.build_frame(pr.T_CAL_GET, seq, b""))
    t0 = time.time()
    while time.time() - t0 < a.sec:
        k += 1
        for f in (lk.poll(k) or []):
            if f.type == pr.T_CAL_REPORT:
                reports.append(f.payload)
            elif f.type == pr.T_TELEM2:
                telem += 1
        time.sleep(0.002)
    lk.stop()

    print("  telemetry frames: %d ; cal reports: %d" % (telem, len(reports)))
    if not reports:
        print("  BAD: no T_CAL_REPORT (0x15) -- sensor path or T_CAL_GET not answered")
        return 1
    p = reports[-1]
    print("  payload len = %d" % len(p))
    if len(p) >= 81:
        raw = u16s(p, 18, 8)
        g2 = u16s(p, 34, 8)
        g3 = u16s(p, 50, 8)
        flags = u16s(p, 66, 8)
        print("  raw[8]   = %s" % raw)
        print("  grp2[8]  = %s" % g2)
        print("  grp3[8]  = %s" % g3)
        print("  flags[8] = %s" % flags)
        # 加权重心（用 raw 与 grp2 各算一次，看哪个是"压线判定"）
        for name, vals in (("raw", raw), ("grp2", g2)):
            tot = sum(vals)
            if tot <= 0:
                continue
            data = sum((i + 1) * v for i, v in enumerate(vals)) / tot
            err = 4.5 - data
            print("  [%s] centroid data=%.3f  err=%.3f  => rate_cmd = -(%g*err) = %+.1f deg/s"
                  % (name, data, err, a.kp, -(a.kp * err)))
        span = max(raw) - min(raw)
        print("  raw spread (max-min) = %d  => %s" % (
            span, "sensor responds to the line OK" if span > 20 else "NO response: all channels alike"))
    else:
        print("  payload too short; hex=%s" % p.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
