"""陀螺数据流验证：让车原地转（单轮驱动），同时逐帧看 yaw 是否在变。

判据：
  · yaw 随车转动连续变化 ⇒ 陀螺正常 ✓ ⇒ 角度环问题在环/参数
  · yaw 几乎不动（车在转） ⇒ 陀螺/数据链有问题 ✗ ⇒ 先修数据链
用法： python A/_tools/gyrochk.py [--sec 3] [--pwm 4000]
"""
from __future__ import annotations

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from angtune2 import Car     # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--sec", type=float, default=3.0)
    ap.add_argument("--pwm", type=int, default=4000)
    a = ap.parse_args()

    car = Car(a.port)
    print("  陀螺数据流验证：静止 1.5s -> 只驱动左轮(%d) %.1fs -> 停" % (a.pwm, a.sec))

    still = car.drive(2, 0, 0, 1.5)
    ys0 = [r[1] for r in still]
    print("  静止段 yaw: %s  (帧 %d)" % ([round(v, 2) for v in ys0[::max(1, len(ys0) // 6)]][:6], len(ys0)))

    rows = car.drive(1, a.pwm, 0, a.sec)
    car.lk.send(__import__("bridge.protocol", fromlist=["x"]).frame_drive(car._n(), 0, 0, 0))
    time.sleep(0.3)
    car.stop()

    if len(rows) < 20:
        print("  转动段数据不足（%d 帧）" % len(rows))
        return 1
    ys = [r[1] for r in rows]
    print("  转动段 yaw: %s  (帧 %d)" % ([round(v, 1) for v in ys[::max(1, len(ys) // 10)]][:10], len(rows)))
    span = max(ys) - min(ys)
    print("  yaw 变化范围 %.1f deg   末-首 %+.1f deg" % (span, ys[-1] - ys[0]))
    print("  => %s" % ("陀螺数据流正常 OK（车转时 yaw 在变）" if span > 5.0
                       else "陀螺 yaw 几乎不动 BAD（车在转但 yaw 不变）-> 查 JY61P 数据链"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
