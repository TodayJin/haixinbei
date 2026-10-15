"""
逐帧采集 + "卡在哪一层"判定（不依赖上位机）。

为什么要它：
  · 每 200ms 打印一行会把波形看成"振荡"（采样混叠）——这里**逐帧全存**；
  · 用**车自己的时间戳** t(ms) 算帧间隔 => 能看出遥测卡顿（实测中位 5ms、最大 210ms）；
  · 把固件自测的 `loop_us`（TIM6 ISR 每拍耗时）一起采下来 =>
      卡顿时 loop_us 也暴涨  => 卡在**中断**里（TIM6/TIM7/串口）
      loop_us 平稳          => 卡在**主循环**（中断照跑，只是遥测发不出去）
    —— 这是**可判定**的判据，不用猜。

用法：
  python A/_tools/trace.py --sec 4                       # 只监视
  python A/_tools/trace.py --sec 2.5 --a -5000 --b 0     # wheels 模式发命令并采
"""
from __future__ import annotations

import argparse
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE = os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console"))
sys.path.insert(0, CONSOLE)

from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402

MODES = {"off": 0, "wheels": 1, "base_turn": 2, "trace": 3}
OUT = os.path.join(HERE, "pid_logs")
os.makedirs(OUT, exist_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--sec", type=float, default=4.0)
    ap.add_argument("--mode", default="wheels")
    ap.add_argument("--a", type=int, default=0)
    ap.add_argument("--b", type=int, default=0)
    ap.add_argument("--keepalive", type=float, default=0.08)
    ap.add_argument("--top", type=int, default=8)
    a = ap.parse_args()

    lk = SerialLink(port=a.port, baud=a.baud)
    lk.start()
    time.sleep(0.4)                      # 只等串口打开
    print("  connected %s" % a.port)
    drv = (a.mode != "off") or a.a or a.b
    if drv:
        # ★先把第一帧发出去再开始采集：否则采集开头那段是"还没下命令"的空闲期，
        #   里面的 host_drive=0 / motor=0 会被误读成"会话被清掉"。
        lk.send(pr.frame_drive(49, MODES.get(a.mode, 1), a.a, a.b))
        time.sleep(0.3)
        print("  drive(%s, %d, %d), resend every %.0fms" % (a.mode, a.a, a.b, a.keepalive * 1000))

    rows = []
    t0 = None
    t_start = time.time()
    last_send = -1e9
    k = 0
    while time.time() - t_start < a.sec:
        now = time.time() - t_start
        if drv and now - last_send >= a.keepalive:
            last_send = now
            lk.send(pr.frame_drive(50, MODES.get(a.mode, 1), a.a, a.b))
        k += 1
        for f in (lk.poll(k) or []):
            if f.type != pr.T_TELEM2:
                continue
            d = pr.decode_telemetry(f.payload)
            if t0 is None:
                t0 = d.get("t", 0)
            fl = d.get("flags")
            if isinstance(fl, dict):
                mot = 1 if fl.get("motor") else 0
                hdr = 1 if fl.get("host_drive") else 0
            else:
                mot = ((fl or 0) >> 4) & 1
                hdr = ((fl or 0) >> 2) & 1
            rows.append((d.get("t", 0) - t0, d.get("vl", 0), d.get("vr", 0),
                         d.get("pwm_l", 0), d.get("pwm_r", 0),
                         d.get("ibat_ma", 0), d.get("vbat_mv", 0),
                         d.get("loop_us", 0), mot, hdr, d.get("watchdog_trips", 0)))
        time.sleep(0.001)
    try:
        lk.send(pr.frame_drive(51, 0, 0, 0))
    except Exception:
        pass
    lk.stop()

    if not rows:
        print("  BAD: no telemetry frames")
        return 1

    ts = [r[0] for r in rows]
    lu = [r[7] for r in rows]
    print("\n=== capture quality ===")
    print("  frames %d, span %.0f ms" % (len(rows), ts[-1] - ts[0]))
    dts = [(ts[i] - ts[i - 1], i) for i in range(1, len(ts)) if ts[i] > ts[i - 1]]
    if dts:
        vals = [d for d, _ in dts]
        med = st.median(vals)
        mx, mxi = max(dts, key=lambda x: x[0])
        print("  dt: median %.1f ms (%.0f Hz), max %.0f ms" % (med, 1000.0 / med, mx))
        if mx > 30:
            print("  *** STALL: %d gaps > 30ms; worst at t=%d ms"
                  % (sum(1 for v in vals if v > 30), ts[mxi]))
            print("  --- around the worst stall ---")
            for i in range(max(1, mxi - 3), min(len(rows), mxi + 4)):
                pdt = ts[i] - ts[i - 1] if i > 0 else 0
                print("      t=%6d  dt=%5d ms  loop_us=%6d  vl=%5d  ibat=%5d"
                      % (ts[i], pdt, rows[i][7], rows[i][1], rows[i][5]))
        top = sorted(dts, key=lambda x: -x[0])[:a.top]
        print("  top-%d gaps: %s" % (a.top, ", ".join("%dms@%d" % (d, ts[i]) for d, i in top)))

    print("\n=== loop_us (ISR duration, self-measured by firmware) ===")
    print("  median %d us, max %d us, p99 %d us" % (
        st.median(lu), max(lu), sorted(lu)[int(len(lu) * 0.99)] if lu else 0))
    over = sum(1 for v in lu if v > 500)
    print("  frames with loop_us > 500us: %d/%d" % (over, len(lu)))
    if len(rows) > 20:
        if over > len(rows) * 0.1:
            print("  => ISR itself is slow => the stall is INSIDE the interrupt")
        else:
            print("  => ISR is fine => the stall is in the MAIN LOOP (telemetry cannot get out)")

    vl = [abs(r[1]) for r in rows]
    print("\n=== signals ===")
    print("  vl(带符号) median %d max %d min %d, nonzero %d%%" % (
        st.median([r[1] for r in rows]), max([r[1] for r in rows]), min([r[1] for r in rows]), round(100.0*sum(1 for v in vl if v>10)/len(vl))))
    print("  ibat median %d max %d mA; vbat med %d min %d mV; pwm_l med %d" % (
        st.median([r[5] for r in rows]), max(r[5] for r in rows),
        st.median([r[6] for r in rows]), min(r[6] for r in rows),
        st.median([r[3] for r in rows])))

    # ★关键判据：pwm 为 0 的那些帧，速度环还在使能吗？
    drv_rows = [r for r in rows if len(r) > 9 and r[9]]     # ★只看"驾驶已生效"的帧
    if drv_rows and len(rows) > 20:
        z = [r for r in drv_rows if abs(r[3]) == 0]
        print("\n=== why is pwm 0? (only frames with host_drive=1) ===")
        print("  driving frames %d/%d ; of them pwm==0: %d (%.0f%%)"
              % (len(drv_rows), len(rows), len(z), 100.0 * len(z) / len(drv_rows)))
        print("  of those pwm==0: motor(enabled)=%d" % sum(1 for r in z if len(r) > 8 and r[8]))
        print("  watchdog_trips at end = %s" % (rows[-1][10] if len(rows[-1]) > 10 else "?"))

    stamp = time.strftime("%Y%m%d%H%M%S")
    path = os.path.join(OUT, "trace_%s.csv" % stamp)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# mode=%s a=%d b=%d sec=%.1f\n" % (a.mode, a.a, a.b, a.sec))
        fh.write("t_ms,vl,vr,pwm_l,pwm_r,ibat_ma,vbat_mv,loop_us,motor,host_drive,trips\n")
        for r in rows:
            fh.write(",".join(str(x) for x in r) + "\n")
    print("\n  saved %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
