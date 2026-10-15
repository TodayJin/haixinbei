"""角度环自动整定：发"转角阶跃" -> 量 yaw 响应 -> 调参 -> 再下发（闭环循环）。

驱动语义： mode=2 (base_turn)，a = 前进速度刻度，b = **相对进入时的累计转角(°)**。
指标（每段阶跃）：
    超调 %       = (峰值 - 目标)/目标 * 100
    上升时间 ms  = 首次到达 90% 目标的时间
    整定时间 ms  = 最后一次离开 ±3% 带的时间
    稳态误差 °   = 末段均值 - 目标
调整规则（一次一个变量）：
    超调 > 25%        -> gyro_rate_kd 加大（阻尼）
    上升太慢(>1200ms) -> gyro_ang_kp 加大
    持续振荡          -> gyro_rate_kp 减小 或 rate_kd 加大
    稳态有静差        -> gyro_ki 加 0.5
用法： python A/_tools/angtune.py [--rounds 4] [--angle 60] [--speed 900]
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
PID = {"gyro_kp": 0x0101, "gyro_ki": 0x0102, "gyro_kd": 0x0103, "gyro_ilimit": 0x0104,
       "gyro_ang_kp": 0x010A, "gyro_rate_kp": 0x010B, "gyro_rate_kd": 0x010C,
       "gyro_rate_max": 0x010D, "gyro_corr_limit": 0x010E, "gyro_corr_slew": 0x010F,
       "gyro_settle_deg": 0x0110, "gyro_settle_ms": 0x0111, "gyro_corr_ref": 0x0112}


class Car:
    def __init__(self):
        self.lk = SerialLink(port=PORT, baud=921600)
        self.lk.start()
        time.sleep(0.6)
        self.seq = 400
        self.k = 0

    def _n(self):
        self.seq += 1
        return self.seq

    def set_params(self, kv):
        for name, v in kv.items():
            pid = PID.get(name)
            if pid is None:
                continue
            self.lk.send(pr.build_frame(pr.T_PARAM_SET, self._n(),
                                        struct.pack("<H", pid) + struct.pack("<f", float(v))))
            time.sleep(0.06)
        time.sleep(0.3)

    def step(self, speed, angle, sec=4.0):
        """先回零(angle=0)，再打阶跃到 angle，采集全过程。"""
        def run(ang, dur):
            rows = []
            t0 = time.time()
            last = -1e9
            while time.time() - t0 < dur:
                now = time.time() - t0
                if now - last >= 0.08:
                    last = now
                    self.lk.send(pr.frame_drive(self._n(), 2, speed, ang))
                self.k += 1
                for f in (self.lk.poll(self.k) or []):
                    if f.type == pr.T_TELEM2:
                        d = pr.decode_telemetry(f.payload)
                        rows.append((now, d.get("yaw", 0.0) / 10.0, d.get("pwm_l", 0), d.get("pwm_r", 0)))
                time.sleep(0.002)
            return rows
        run(0, 1.5)                      # 回零
        rows = run(angle, sec)           # 阶跃
        self.lk.send(pr.frame_drive(self._n(), 2, 0, 0))
        time.sleep(0.3)
        return rows

    def stop(self):
        try:
            self.lk.send(pr.frame_drive(self._n(), 0, 0, 0))
        except Exception:
            pass
        self.lk.stop()


def metrics(rows, target):
    if len(rows) < 30:
        return None
    t = [r[0] for r in rows]
    y = [r[1] for r in rows]
    peak = max(y) if target > 0 else min(y)
    over = (abs(peak) - abs(target)) / abs(target) * 100.0
    t10 = t[0]
    for i in range(len(y)):
        if abs(y[i]) >= 0.9 * abs(target):
            t10 = t[i]
            break
    rise = (t10 - t[0]) * 1000.0
    band = 0.03 * abs(target)
    t_set = 0.0
    for i in range(len(y)):
        if abs(y[i] - target) > band:
            t_set = t[i]
    settle = (t_set - t[0]) * 1000.0
    tail = y[int(len(y) * 0.8):]
    ss = st.median(tail) - target
    jit = st.pstdev(tail)
    return {"peak": peak, "over": over, "rise": rise, "settle": settle, "ss": ss, "jit": jit}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--angle", type=float, default=60.0)
    ap.add_argument("--speed", type=int, default=900)
    a = ap.parse_args()

    car = Car()
    p = {"gyro_kp": 150.0, "gyro_ki": 0.0, "gyro_kd": 15.0, "gyro_ilimit": 1000.0,
         "gyro_ang_kp": 10.0, "gyro_rate_kp": 40.0, "gyro_rate_kd": 0.0,
         "gyro_rate_max": 120.0, "gyro_corr_limit": 3000.0, "gyro_corr_slew": 300000.0,
         "gyro_settle_deg": 0.8, "gyro_settle_ms": 150.0, "gyro_corr_ref": 3000.0}
    car.set_params(p)
    print("  角度环整定：阶跃 %.0f°，速度 %d，每段 4s" % (a.angle, a.speed))
    print("  起点: ang_kp=10 rate_kp=40 rate_kd=0 gyro_kp=150 gyro_kd=15\n")
    for rd in range(1, a.rounds + 1):
        print("  ---- 第 %d 轮  ang_kp=%g rate_kp=%g rate_kd=%g gyro_ki=%g ----"
              % (rd, p["gyro_ang_kp"], p["gyro_rate_kp"], p["gyro_rate_kd"], p["gyro_ki"]))
        rows = car.step(a.speed, a.angle)
        m = metrics(rows, a.angle)
        if m is None:
            print("     (无数据)")
            break
        print("     峰值 %6.1f°  超调 %5.1f%%  上升 %5.0fms  整定 %5.0fms  静差 %5.2f°  抖动 %4.2f°"
              % (m["peak"], m["over"], m["rise"], m["settle"], m["ss"], m["jit"]))
        if m["over"] > 25.0:
            p["gyro_rate_kd"] = min(50.0, p["gyro_rate_kd"] + 3.0)
            print("     -> 超调大 ⇒ rate_kd 加到 %g" % p["gyro_rate_kd"])
        elif m["rise"] > 1200.0:
            p["gyro_ang_kp"] = min(20.0, p["gyro_ang_kp"] + 1.5)
            print("     -> 上升慢 ⇒ ang_kp 加到 %g" % p["gyro_ang_kp"])
        elif m["jit"] > 2.0:
            p["gyro_rate_kp"] = max(10.0, p["gyro_rate_kp"] * 0.8)
            print("     -> 振荡 ⇒ rate_kp 降到 %g" % p["gyro_rate_kp"])
        elif abs(m["ss"]) > 1.5 and p["gyro_ki"] < 5.0:
            p["gyro_ki"] = round(p["gyro_ki"] + 0.5, 2)
            print("     -> 有静差 ⇒ gyro_ki 加到 %g" % p["gyro_ki"])
        else:
            print("     -> 已可用 ⇒ 结束")
            break
        car.set_params(p)
        time.sleep(1.2)
    car.stop()
    print("\n  ===== 最终角度环参数 =====")
    for k in ("gyro_ang_kp", "gyro_rate_kp", "gyro_rate_kd", "gyro_ki", "gyro_kp", "gyro_kd", "gyro_rate_max"):
        print("     %-16s = %g" % (k, p[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
