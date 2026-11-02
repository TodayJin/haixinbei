"""参数读写工具（ASCII 输出，逐条确认）—— 取代 car.py 的 set/乱码输出。

用法：
  python A/_tools/param.py get                     # 打印车上全部关键参数真实值
  python A/_tools/param.py set speed_kp=40 gyro_rate_kd=0
  python A/_tools/param.py save                    # 固化到 NVRAM（等保存窗口）
  python A/_tools/param.py get speed_kp gyro_rate_kd   # 只查指定几个

设计要点（这轮血的教训）：
  · **输出全 ASCII**（控制台是 GBK，中文/符号会乱码，导致我误判"写没写进去"）
  · set 每条形如： SET speed_kp = 40  [OK 回读=40]   —— 失败打印 [FAIL ...]
  · get 每条形如： GET speed_kp = 40  (0x0601)
  · 每条 set 都等 T_PARAM_REPORT（超时 0.5s，重试 3 次）
"""
from __future__ import annotations

import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402

PARAMS = {
    # id : name
    0x0305: "counts_per_rev", 0x0306: "wheel_dia", 0x0307: "wheel_base", 0x0308: "speed_full_counts",
    0x0601: "speed_kp", 0x0602: "speed_ki", 0x0603: "speed_kd", 0x0604: "speed_ilimit",
    0x0605: "speed_ff_slope", 0x0606: "speed_ff_fric",
    0x0201: "line_kp", 0x0202: "line_ki", 0x0203: "line_kd", 0x0204: "line_ilimit",
    0x0101: "gyro_kp", 0x0102: "gyro_ki", 0x0103: "gyro_kd", 0x0104: "gyro_ilimit",
    0x010A: "gyro_ang_kp", 0x010B: "gyro_rate_kp", 0x010C: "gyro_rate_kd",
    0x010D: "gyro_rate_max", 0x010E: "gyro_corr_limit", 0x010F: "gyro_corr_slew",
    0x0110: "gyro_settle_deg", 0x0111: "gyro_settle_ms", 0x0112: "gyro_corr_ref",
    0x020A: "trace_rate_max",
    0x0501: "vbat_nominal", 0x0503: "vbat_low",
}
BY_NAME = {v: k for k, v in PARAMS.items()}
ORDER = [0x0305, 0x0306, 0x0307, 0x0308, 0x0601, 0x0602, 0x0603, 0x0604, 0x0605, 0x0606,
         0x0101, 0x0102, 0x0103, 0x0104, 0x010A, 0x010B, 0x010C, 0x010D, 0x010E, 0x010F,
         0x0110, 0x0111, 0x0112]


class P:
    def __init__(self, port):
        self.lk = SerialLink(port=port, baud=921600)
        self.lk.start()
        time.sleep(0.7)
        self.seq = 1000
        self.k = 0

    def _n(self):
        self.seq += 1
        return self.seq

    def _wait_report(self, pid, sec=0.5):
        t0 = time.time()
        while time.time() - t0 < sec:
            self.k += 1
            for f in (self.lk.poll(self.k) or []):
                if f.type == pr.T_PARAM_REPORT:
                    try:
                        rid, rv = pr.decode_param_report(f.payload)
                        if rid == pid:
                            return rv
                    except Exception:
                        pass
            time.sleep(0.002)
        return None

    def get(self, pid):
        self.lk.send(pr.build_frame(pr.T_PARAM_GET, self._n(), struct.pack("<H", pid)))
        return self._wait_report(pid, 0.5)

    def set(self, pid, val, tries=3):
        for _ in range(tries):
            self.lk.send(pr.build_frame(pr.T_PARAM_SET, self._n(),
                                        struct.pack("<H", pid) + struct.pack("<f", float(val))))
            rv = self._wait_report(pid, 0.5)
            if rv is not None:
                return rv
        return None

    def save(self):
        self.lk.send(pr.build_frame(pr.T_PARAM_SAVE, self._n(), b""))
        time.sleep(2.5)

    def close(self):
        self.lk.stop()


def main() -> int:
    port = os.environ.get("CAR_PORT", "COM4")
    args = sys.argv[1:]
    if not args:
        args = ["get"]
    cmd = args[0].lower()
    car = P(port)
    rc = 0
    try:
        if cmd == "get":
            names = args[1:] or None
            ids = [BY_NAME[n] for n in names] if names else ORDER
            print("--- GET (car truth) ---")
            for pid in ids:
                v = car.get(pid)
                if v is None:
                    print("GET %-18s = (no reply)  0x%04X" % (PARAMS.get(pid, "?"), pid))
                    rc = 1
                else:
                    print("GET %-18s = %-12g 0x%04X" % (PARAMS.get(pid, "?"), v, pid))
        elif cmd == "set":
            print("--- SET ---")
            for kv in args[1:]:
                if "=" not in kv:
                    print("SET bad arg %s" % kv); rc = 1; continue
                k, s = kv.split("=", 1)
                k = k.strip()
                if k not in BY_NAME:
                    print("SET %-18s [FAIL unknown name]" % k); rc = 1; continue
                try:
                    val = float(s)
                except ValueError:
                    print("SET %-18s [FAIL bad value %s]" % (k, s)); rc = 1; continue
                rv = car.set(BY_NAME[k], val)
                if rv is None:
                    print("SET %-18s = %-12g [FAIL no report]" % (k, val)); rc = 1
                elif abs(rv - val) > max(1e-3, abs(val) * 1e-4):
                    print("SET %-18s = %-12g [OK but readback %g]" % (k, val, rv)); rc = 1
                else:
                    print("SET %-18s = %-12g [OK]" % (k, val))
            car.save()
            print("--- SAVE done (waited 2.5s) ---")
        elif cmd == "save":
            car.save()
            print("--- SAVE done ---")
            for pid in ORDER:
                v = car.get(pid)
                if v is not None:
                    print("GET %-18s = %-12g" % (PARAMS.get(pid, "?"), v))
        else:
            print("unknown cmd %s" % cmd); rc = 2
    finally:
        car.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
