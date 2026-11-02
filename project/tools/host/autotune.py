"""负载工况下的速度环自动整定：下发 -> 采集 -> 调参 -> 再下发（闭环循环）。

要点：
  · 车放在地上（带载）⇒ 每段只跑 2.5s，且**前/后交替**，车不会跑远；
  · 单进程独占串口：驱动 + 采集 + 改参数都在这里做；
  · 指标：稳态速度（后 40% 帧中位）、抖动 σ、跟随比（实测/期望）；
  · 调整规则简单明确（可追溯）：低速够不到就加 ff_fric，跑过头就减；
    抖得厉害就减 kp；跟得上且稳了，再加 ki 消静差。
用法： python A/_tools/autotune.py [--rounds 5]
"""
from __future__ import annotations

import argparse
import os
import statistics as st
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402

PORT = os.environ.get("CAR_PORT", "COM4")
K_MM = 28.05          # 1 计数/5ms = 28.05 mm/s（cpr=1456, dia=65）
FULL = 26.0
PID = {"speed_kp": 0x0601, "speed_ki": 0x0602, "speed_kd": 0x0603,
       "speed_ilimit": 0x0604, "speed_ff_slope": 0x0605, "speed_ff_fric": 0x0606,
       "speed_full_counts": 0x0308}
TARGETS = [1500, 3000, 4500]
SEC = 2.5


def expected_mmps(T: float) -> float:
    return T * FULL / 7200.0 * K_MM


class Car:
    def __init__(self):
        self.lk = SerialLink(port=PORT, baud=921600)
        self.lk.start()
        time.sleep(0.6)
        self.seq = 300
        self.k = 0

    def _n(self):
        self.seq += 1
        return self.seq

    def set_params(self, kv: dict) -> None:
        for name, v in kv.items():
            pid = PID.get(name)
            if pid is None:
                continue
            self.lk.send(pr.build_frame(pr.T_PARAM_SET, self._n(),
                                        struct.pack("<H", pid) + struct.pack("<f", float(v))))
            time.sleep(0.06)
        # 固化（可选）：不发 SAVE，避免频繁擦写
        time.sleep(0.3)

    def run(self, a: int, b: int, sec: float):
        rows = []
        t0 = time.time()
        last = -1e9
        while time.time() - t0 < sec:
            now = time.time() - t0
            if now - last >= 0.08:
                last = now
                self.lk.send(pr.frame_drive(self._n(), 1, a, b))
            self.k += 1
            for f in (self.lk.poll(self.k) or []):
                if f.type == pr.T_TELEM2:
                    d = pr.decode_telemetry(f.payload)
                    rows.append((now, d.get("vl", 0), d.get("vr", 0),
                                 d.get("pwm_l", 0), d.get("pwm_r", 0), d.get("ibat_ma", 0)))
            time.sleep(0.002)
        self.lk.send(pr.frame_drive(self._n(), 0, 0, 0))
        time.sleep(0.4)
        tail = [r for r in rows if r[0] > sec * 0.55]
        return tail

    def stop(self):
        try:
            self.lk.send(pr.frame_drive(self._n(), 0, 0, 0))
        except Exception:
            pass
        self.lk.stop()


def measure(car: Car, T: int):
    """前/后各一段（交替，车不会跑远），取两段稳态幅值的平均。"""
    out = []
    for sign in (1, -1):
        a, b = T * sign, 0          # 只左轮，负载下更稳
        tail = car.run(a, b, SEC)
        if len(tail) < 15:
            out.append(None)
            continue
        v = st.median(r[1] for r in tail)
        sd = st.pstdev([r[1] for r in tail])
        pwm = st.median(abs(r[3]) for r in tail)
        out.append({"v": v, "sd": sd, "pwm": pwm, "n": len(tail)})
    if any(x is None for x in out):
        return None
    v = (out[0]["v"] - out[1]["v"]) / 2.0
    sd = (out[0]["sd"] + out[1]["sd"]) / 2.0
    pwm = (out[0]["pwm"] + out[1]["pwm"]) / 2.0
    return {"v": v, "sd": sd, "pwm": pwm, "n": out[0]["n"] + out[1]["n"]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5)
    a = ap.parse_args()

    car = Car()
    p = {"speed_kp": 40.0, "speed_ki": 0.0, "speed_kd": 0.0,
         "speed_ilimit": 3000.0, "speed_ff_slope": 0.33, "speed_ff_fric": 3990.0,
         "speed_full_counts": 26.0}
    car.set_params(p)
    print("  起点: kp=40 ki=0 kd=0 slope=0.33 fric=3990 full=26   （负载工况，每段 %.1fs）" % SEC)
    print("  期望: 1500->%.0f  3000->%.0f  4500->%.0f mm/s\n"
          % (expected_mmps(1500), expected_mmps(3000), expected_mmps(4500)))

    hist = []
    for rd in range(1, a.rounds + 1):
        print("  ---- 第 %d 轮  kp=%g ki=%g kd=%g slope=%g fric=%g ----"
              % (rd, p["speed_kp"], p["speed_ki"], p["speed_kd"], p["speed_ff_slope"], p["speed_ff_fric"]))
        res = {}
        for T in TARGETS:
            m = measure(car, T)
            if m is None:
                print("     目标 %5d  (无数据)" % T)
                continue
            exp = expected_mmps(T)
            ratio = m["v"] / exp if exp else 0
            res[T] = (m, ratio, exp)
            print("     目标 %5d  期望 %4.0f  实测 %6.0f  比 %5.2f  抖动σ %4.0f  pwm %5.0f"
                  % (T, exp, m["v"], ratio, m["sd"], m["pwm"]))
        if not res:
            break
        hist.append((dict(p), {k: (v[0]["v"], v[1]) for k, v in res.items()}))

        ratios = [v[1] for v in res.values()]
        sds = [v[0]["sd"] / max(1.0, abs(v[0]["v"])) for v in res.values()]
        avg = sum(ratios) / len(ratios)
        jit = sum(sds) / len(sds)
        # --- 调整规则（一次一个变量）---
        if jit > 0.22:
            p["speed_kp"] = max(10.0, p["speed_kp"] * 0.7)
            print("     -> 抖动大(%.2f) ⇒ kp 降到 %g" % (jit, p["speed_kp"]))
        elif avg < 0.80:
            p["speed_ff_fric"] = min(6000.0, p["speed_ff_fric"] + 350.0)
            print("     -> 整体偏慢(%.2f) ⇒ ff_fric 加到 %g" % (avg, p["speed_ff_fric"]))
        elif avg > 1.25:
            p["speed_ff_fric"] = max(800.0, p["speed_ff_fric"] - 350.0)
            print("     -> 整体偏快(%.2f) ⇒ ff_fric 减到 %g" % (avg, p["speed_ff_fric"]))
        elif abs(res[TARGETS[0]][1] - 1.0) > 0.15 and p["speed_ki"] < 1.0:
            p["speed_ki"] = round(p["speed_ki"] + 0.3, 2)
            print("     -> 低速段有静差(%.2f) ⇒ 加 ki 到 %g" % (res[TARGETS[0]][1], p["speed_ki"]))
        else:
            print("     -> 已可用（平均跟随比 %.2f，抖动 %.2f）⇒ 结束" % (avg, jit))
            break
        car.set_params(p)
        time.sleep(1.5)

    car.stop()
    print("\n  ===== 最终参数 =====")
    for k, v in p.items():
        print("     %-18s = %g" % (k, v))
    print("\n  ===== 历史 =====")
    for i, (pp, rr) in enumerate(hist, 1):
        print("   第%d轮 kp=%-4g ki=%-4g fric=%-5g  " % (i, pp["speed_kp"], pp["speed_ki"], pp["speed_ff_fric"])
              + "  ".join("%d:%.0f(%.2f)" % (T, v[0], v[1]) for T, v in sorted(rr.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
