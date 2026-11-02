"""
速度环标定：单轮、正确符号（左 +T / 右 −T 都是"前进"），逐目标量"目标 ↔ 实测"。

判据（每档）：
  期望计数 = T * speed_full_counts / 7200      （环内部的期望，计数/5ms）
  实测计数 = |vl| / (1000/5 / cpr * pi * dia)   （从 mm/s 反算，mm/s→计数的逆运算）
  跟随比   = 实测 / 期望     （1.00 = 刻度正确）
用法： python A/_tools/cal.py [--full 45] [--cpr 1456] [--dia 65] [--targets 1500,2000,2500,3000]
"""
from __future__ import annotations

import argparse
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402


def run_one(lk, seq, a, b, sec, keepalive=0.08):
    rows = []
    t0 = time.time()
    last = -1e9
    k = 0
    while time.time() - t0 < sec:
        now = time.time() - t0
        if now - last >= keepalive:
            last = now
            lk.send(pr.frame_drive(seq, 1, a, b))
        k += 1
        for f in (lk.poll(k) or []):
            if f.type == pr.T_TELEM2:
                d = pr.decode_telemetry(f.payload)
                rows.append((now, d.get("vl", 0), d.get("vr", 0), d.get("pwm_l", 0),
                             d.get("pwm_r", 0), d.get("ibat_ma", 0)))
        time.sleep(0.002)
    # 只取后半段（已经稳了）
    rows = [r for r in rows if r[0] > sec * 0.5]
    return rows


def med(rows, idx):
    if not rows:
        return 0
    return st.median(r[idx] for r in rows)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--full", type=float, default=45.0)
    ap.add_argument("--cpr", type=float, default=1456.0)
    ap.add_argument("--dia", type=float, default=65.0)
    ap.add_argument("--sec", type=float, default=2.2)
    ap.add_argument("--targets", default="1500,2000,2500,3000")
    a = ap.parse_args()
    targets = [int(x) for x in a.targets.split(",")]

    # mm/s -> 计数/5ms 的比例
    k_mm_to_cnt = (1000.0 / 5.0) / a.cpr * 3.14159265 * a.dia     # 1 计数/5ms = ? mm/s

    lk = SerialLink(port=a.port, baud=921600)
    lk.start()
    time.sleep(0.5)
    print("  已连接 %s   full=%g cpr=%g dia=%g" % (a.port, a.full, a.cpr, a.dia))
    print("  1 计数/5ms = %.2f mm/s" % k_mm_to_cnt)

    out = []
    for wheel in ("左(+T,0)", "右(0,-T)"):
        print("\n=== %s ===" % wheel)
        print("   目标  期望计数  实测计数  跟随比   |v|中位  pwm中位  ibat中位  零速帧")
        for T in targets:
            av, bv = (T, 0) if wheel.startswith("左") else (0, -T)
            rows = run_one(lk, 100 + T // 10, av, bv, a.sec)
            if not rows:
                print("   %5d  (无数据)" % T)
                continue
            v = med(rows, 1) if wheel.startswith("左") else med(rows, 2)
            v = abs(v)
            exp = T * a.full / 7200.0
            got = v / k_mm_to_cnt
            ratio = (got / exp) if exp > 0 else 0
            pwm = med(rows, 3) if wheel.startswith("左") else med(rows, 4)
            ib = med(rows, 5)
            nz = sum(1 for r in rows if abs(r[1] if wheel.startswith("左") else r[2]) < 10)
            print("   %5d  %8.2f  %8.2f  %6.2f  %6.0f  %7.0f  %7.0f  %3d/%3d"
                  % (T, exp, got, ratio, v, pwm, ib, nz, len(rows)))
            out.append((wheel, T, exp, got, ratio))

    try:
        lk.send(pr.frame_drive(999, 0, 0, 0))
    except Exception:
        pass
    lk.stop()

    print("\n=== 汇总：跟随比（1.00 = 刻度对）===")
    for w, T, exp, got, ratio in out:
        print("   %-10s T=%5d  期望 %6.2f  实测 %6.2f  比 %5.2f  %s"
              % (w, T, exp, got, ratio, "OK" if 0.9 <= ratio <= 1.1 else "偏差大"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
