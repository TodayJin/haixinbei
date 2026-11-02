"""角度环"修正量"直查：base_turn 模式下看 pwm_l / pwm_r 的差（差 = 转向修正量）。

判据：
  · pwm_l 与 pwm_r 有**明显反向差** ⇒ 修正量在加 ✓ ⇒ 问题在修正量的**大小/方向**
  · pwm_l ≈ pwm_r ⇒ 修正量≈0 ✗ ⇒ 角度环没往电机上加东西（查 gyro_control_enabled/模式）
用法： python A/_tools/angchk.py [--angle 60] [--speed 900]
"""
from __future__ import annotations

import argparse
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge import protocol as pr     # noqa: E402
from angtune2 import Car              # noqa: E402


def drive_full(car: Car, mode: int, x: int, y: int, sec: float):
    rows = []
    t0 = time.time()
    last = -1e9
    while time.time() - t0 < sec:
        now = time.time() - t0
        if now - last >= 0.08:
            last = now
            car.lk.send(pr.frame_drive(car._n(), mode, x, y))
        car.k += 1
        for f in (car.lk.poll(car.k) or []):
            if f.type == pr.T_TELEM2:
                d = pr.decode_telemetry(f.payload)
                rows.append((now, d.get("yaw", 0.0) / 10.0, d.get("pwm_l", 0),
                             d.get("pwm_r", 0), d.get("vl", 0), d.get("vr", 0)))
        time.sleep(0.002)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--angle", type=float, default=60.0)
    ap.add_argument("--speed", type=int, default=900)
    a = ap.parse_args()

    car = Car(a.port)
    print("  base_turn 修正量直查：a=%d(速度) b=%.0f(转角)" % (a.speed, a.angle))
    drive_full(car, 2, a.speed, 0, 1.3)                       # 回零
    rows = drive_full(car, 2, a.speed, int(a.angle), 3.5)      # 阶跃
    car.lk.send(pr.frame_drive(car._n(), 0, 0, 0))
    time.sleep(0.3)
    car.stop()

    if len(rows) < 30:
        print("  数据不足（%d 帧）" % len(rows))
        return 1
    print("    t     yaw   pwm_l  pwm_r   差(修)   vl    vr")
    for r in rows[::max(1, len(rows) // 12)]:
        print("  %5.2f  %6.1f %6.0f %6.0f %7.0f %6.0f %6.0f"
              % (r[0], r[1], r[2], r[3], r[2] - r[3], r[4], r[5]))
    dl = st.median([r[2] - r[3] for r in rows])
    y0 = st.median([r[1] for r in rows[:8]])
    y1 = st.median([r[1] for r in rows[-8:]])
    print()
    print("  中位 pwm_l=%.0f  pwm_r=%.0f  **差=%.0f**" % (
        st.median([r[2] for r in rows]), st.median([r[3] for r in rows]), dl))
    print("  yaw: 起点 %.1f  终点 %.1f  实际转角 %+.1f  （目标 %+.0f）" % (y0, y1, y1 - y0, a.angle))
    if abs(dl) < 200:
        print("  => 修正量≈0 BAD：角度环没把转向量加到电机上 ✗")
    else:
        print("  => 修正量在加 OK（差 %.0f）：问题在修正量大小/方向 或 车转不动（负载）" % dl)
    return 0


if __name__ == "__main__":
    sys.exit(main())
