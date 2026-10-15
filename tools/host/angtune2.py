"""角度环自动整定 v2（自包含、单一串口、验证式下发）。

与 v1 的区别（v1 失败的教训）：
  · **不用内联 python -c**（引号/globals 拼接必炸）—— 一律写成文件；
  · setp() **逐个下发并等 T_PARAM_REPORT 回读确认**（超时重试 2 次），失败会打印；
  · 采集用已验证的 poll 模式（先 self.k += 1 再 poll，sleep 2ms）。
整定对象：转角阶跃（mode=2 base_turn，a=速度刻度，b=累计转角deg）
指标：起点/峰值/超调%/上升ms/整定ms/终值/静差/抖动
用法： python A/_tools/angtune2.py --kd 0,3,6,10 --angle 60 --speed 900
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

PID = {"gyro_kp": 0x0101, "gyro_ki": 0x0102, "gyro_kd": 0x0103, "gyro_ilimit": 0x0104,
       "gyro_ang_kp": 0x010A, "gyro_rate_kp": 0x010B, "gyro_rate_kd": 0x010C,
       "gyro_rate_max": 0x010D, "gyro_corr_limit": 0x010E, "gyro_corr_slew": 0x010F,
       "gyro_settle_deg": 0x0110, "gyro_settle_ms": 0x0111, "gyro_corr_ref": 0x0112}


class Car:
    def __init__(self, port: str):
        self.lk = SerialLink(port=port, baud=921600)
        self.lk.start()
        time.sleep(0.7)
        self.seq = 900
        self.k = 0
        self.fail = []

    def _n(self) -> int:
        self.seq += 1
        return self.seq

    def poll(self, sec: float, keep=("telem",)):
        """收 sec 秒；返回 TELEM2 解码后的列表。"""
        out = []
        t0 = time.time()
        while time.time() - t0 < sec:
            self.k += 1
            for f in (self.lk.poll(self.k) or []):
                if f.type == pr.T_TELEM2:
                    out.append(pr.decode_telemetry(f.payload))
            time.sleep(0.002)
        return out

    def setp(self, name: str, val: float, tries: int = 3) -> bool:
        pid = PID.get(name)
        if pid is None:
            print("     !! 未登记的参数名 %s" % name)
            return False
        for _ in range(tries):
            sq = self._n()
            self.lk.send(pr.build_frame(pr.T_PARAM_SET, sq,
                                        struct.pack("<H", pid) + struct.pack("<f", float(val))))
            t0 = time.time()
            got = None
            while time.time() - t0 < 0.45:
                self.k += 1
                for f in (self.lk.poll(self.k) or []):
                    if f.type == pr.T_PARAM_REPORT:
                        try:
                            rid, rv = pr.decode_param_report(f.payload)
                            if rid == pid:
                                got = rv
                        except Exception:
                            pass
                if got is not None:
                    break
                time.sleep(0.002)
            if got is not None and abs(got - float(val)) <= max(1e-3, abs(val) * 1e-4):
                return True
        self.fail.append(name)
        print("     !! 下发失败 %s=%g" % (name, val))
        return False

    def drive(self, mode: int, a: int, b: int, sec: float, collect: bool = True):
        rows = []
        t0 = time.time()
        last = -1e9
        while time.time() - t0 < sec:
            now = time.time() - t0
            if now - last >= 0.08:
                last = now
                self.lk.send(pr.frame_drive(self._n(), mode, a, b))
            self.k += 1
            for f in (self.lk.poll(self.k) or []):
                if f.type == pr.T_TELEM2:
                    d = pr.decode_telemetry(f.payload)
                    rows.append((now, d.get("yaw", 0.0) / 10.0))
            time.sleep(0.002)
        return rows

    def step_test(self, angle: float, speed: int, hold: float = 3.5):
        self.drive(2, speed, 0, 1.3)             # 回零
        rows = self.drive(2, speed, int(angle), hold)
        self.lk.send(pr.frame_drive(self._n(), 0, 0, 0))
        time.sleep(0.4)
        return rows

    def stop(self):
        try:
            self.lk.send(pr.frame_drive(self._n(), 0, 0, 0))
        except Exception:
            pass
        self.lk.stop()


def report(rows, angle):
    if len(rows) < 40:
        return None
    t = [r[0] for r in rows]
    y = [r[1] for r in rows]
    y0 = st.median(y[:8])
    rel = [v - y0 for v in y]                   # 相对起点的转角
    peak = max(rel) if angle > 0 else min(rel)
    over = (abs(peak) - abs(angle)) / abs(angle) * 100.0
    rise = None
    for i, v in enumerate(rel):
        if abs(v) >= 0.9 * abs(angle):
            rise = (t[i] - t[0]) * 1000.0
            break
    band = 0.05 * abs(angle)
    tset = 0.0
    for i, v in enumerate(rel):
        if abs(v - angle) > band:
            tset = t[i]
    settle = (tset - t[0]) * 1000.0
    tail = rel[int(len(rel) * 0.8):]
    return {"y0": y0, "peak": peak, "over": over, "rise": rise,
            "settle": settle, "final": st.median(tail),
            "ss": st.median(tail) - angle, "jit": st.pstdev(tail)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--angle", type=float, default=60.0)
    ap.add_argument("--speed", type=int, default=900)
    ap.add_argument("--kd", default="0,3,6,10")
    a = ap.parse_args()
    kds = [float(x) for x in a.kd.split(",")]

    car = Car(a.port)
    print("  角度环整定 v2：%.0fdeg 阶跃，速度 %d，逐档改 gyro_rate_kd" % (a.angle, a.speed))
    base = {"gyro_kp": 150.0, "gyro_ki": 0.0, "gyro_kd": 15.0, "gyro_ilimit": 1000.0,
            "gyro_ang_kp": 10.0, "gyro_rate_kp": 40.0, "gyro_rate_kd": 0.0,
            "gyro_rate_max": 120.0, "gyro_corr_limit": 3000.0, "gyro_corr_slew": 300000.0,
            "gyro_settle_deg": 0.8, "gyro_settle_ms": 150.0, "gyro_corr_ref": 3000.0}
    ok = 0
    for k, v in base.items():
        if car.setp(k, v):
            ok += 1
    print("  基线下发成功 %d/%d" % (ok, len(base)))
    t = car.poll(1.0)
    print("  下发后遥测：%d 帧/秒 OK" % len(t))
    print()
    print("   gyro_rate_kd | 起点   峰值  超调%%  上升ms  整定ms  终值   静差   抖动")
    print("  --------------+------------------------------------------------------")
    for kd in kds:
        car.setp("gyro_rate_kd", kd)
        rows = car.step_test(a.angle, a.speed)
        m = report(rows, a.angle)
        if m is None:
            print("   %10g   | (数据不足 %d 帧)" % (kd, len(rows)))
            continue
        print("   %10g   | %5.1f %6.1f %6.1f %7s %7.0f %6.1f %6.2f %6.2f"
              % (kd, m["y0"], m["peak"], m["over"],
                 ("%.0f" % m["rise"]) if m["rise"] else "  -",
                 m["settle"], m["final"], m["ss"], m["jit"]))
    car.stop()
    if car.fail:
        print("\n  下发失败的参数：%s" % ", ".join(car.fail))
    print("\n  判据：超调<15%% / 整定<1.2s / 静差<1deg / 抖动<1deg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
