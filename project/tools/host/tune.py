"""速度环自动扫参：设定参数 -> 单轮驱动 -> 采集 -> 出指标表。

一次连一个串口，参数用 car.py set 下发（子进程，写完就释放串口）。
指标：稳态速度（后 40% 帧的中位数）、抖动（标准差）、跟随比（实测/期望）。
用法： python A/_tools/tune.py
"""
from __future__ import annotations

import os
import statistics as st
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402

CAR = os.path.join(HERE, "car.py")
PORT = os.environ.get("CAR_PORT", "COM4")
FULL = 26.0
K_MM = 28.05          # 1 计数/5ms = 28.05 mm/s（cpr=1456, dia=65）

CONFIGS = [
    (20.0, 0.0), (40.0, 0.0), (60.0, 0.0), (90.0, 0.0),
    (60.0, 0.5), (60.0, 1.0),
]
TARGETS = [1000, 2000, 3000, 4000]


def set_params(kv: dict) -> None:
    args = [sys.executable, CAR, "set"] + ["%s=%s" % (k, v) for k, v in kv.items()]
    subprocess.run(args, capture_output=True, text=True, timeout=120)
    time.sleep(2.5)          # 等固化窗口（nvram 1.5s 防抖 + 余量）


def drive_collect(a: int, b: int, sec: float = 1.8):
    lk = SerialLink(port=PORT, baud=921600)
    lk.start()
    time.sleep(0.35)
    rows = []
    t0 = time.time()
    last = -1e9
    k = 0
    while time.time() - t0 < sec:
        now = time.time() - t0
        if now - last >= 0.08:
            last = now
            lk.send(pr.frame_drive(50, 1, a, b))
        k += 1
        for f in (lk.poll(k) or []):
            if f.type == pr.T_TELEM2:
                d = pr.decode_telemetry(f.payload)
                rows.append((now, d.get("vl", 0), d.get("vr", 0), d.get("pwm_l", 0), d.get("ibat_ma", 0)))
        time.sleep(0.002)
    try:
        lk.send(pr.frame_drive(51, 0, 0, 0))
    except Exception:
        pass
    lk.stop()
    tail = [r for r in rows if r[0] > sec * 0.6]
    return tail


def main() -> int:
    print("  刻度 full_counts=%g  1计数/5ms=%.2f mm/s" % (FULL, K_MM))
    set_params({"speed_full_counts": 26, "speed_kd": 0, "speed_ff_slope": 0.662, "speed_ff_fric": 1000})
    print("  基线：ki=0 kd=0 ff=0.662/1000 full=26\n")
    print("   kp   ki | 目标  期望  实测   跟随比  抖动σ  pwm中位")
    print("  ---------+------------------------------------------------")
    best = []
    for kp, ki in CONFIGS:
        set_params({"speed_kp": kp, "speed_ki": ki})
        for T in TARGETS:
            tail = drive_collect(T, 0)
            if len(tail) < 10:
                print("  %4g %4g | %5d  (无数据)" % (kp, ki, T))
                continue
            v = st.median(r[1] for r in tail)
            sd = st.pstdev([r[1] for r in tail])
            pwm = st.median(abs(r[3]) for r in tail)
            exp = T * FULL / 7200.0 * K_MM
            ratio = v / exp if exp else 0
            print("  %4g %4g | %5d %5.0f %6.0f %6.2f  %5.0f  %6.0f"
                  % (kp, ki, T, exp, v, ratio, sd, pwm))
            if 0.85 <= ratio <= 1.15 and sd < 120:
                best.append((abs(1 - ratio), sd, kp, ki, T, v, exp))
    print()
    if best:
        best.sort()
        d, sd, kp, ki, T, v, exp = best[0]
        print("  ★ 最优: kp=%g ki=%g （目标 %d -> 实测 %.0f，期望 %.0f，抖动 %.0f）" % (kp, ki, T, v, exp, sd))
    else:
        print("  ✗ 没有组合满足(跟随比 0.85~1.15 且 抖动<120)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
