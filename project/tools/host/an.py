"""分析 trace.py 存下的逐帧 CSV：对比"在转"与"没转"两类的 ibat/vbat/pwm。

用途：判断"电机不转"到底是
  (a) 驱动/固件把电断了   => 不转时 ibat≈静止电流、pwm 还在
  (b) 电机通电但转不动     => 不转时 ibat 高（堵转电流）
  (c) 编码器丢计数         => 不转时 ibat 与在转时**一样高**
用法： python A/_tools/an.py [csv路径]   不给就用 pid_logs 里最新的
"""
import glob
import os
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "pid_logs")


def main() -> int:
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        fs = sorted(glob.glob(os.path.join(LOGS, "trace_*.csv")))
        if not fs:
            print("no trace csv")
            return 1
        path = fs[-1]
    print("  file: %s" % os.path.basename(path))

    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#") or line.startswith("t_ms"):
                continue
            v = line.strip().split(",")
            if len(v) >= 7:
                rows.append([int(float(x)) for x in v[:7]])
    if not rows:
        print("  empty")
        return 1

    mov = [r for r in rows if abs(r[1]) > 50]          # 在转
    stp = [r for r in rows if abs(r[1]) <= 10]         # 没转
    mid = [r for r in rows if 10 < abs(r[1]) <= 50]

    def s(name, g):
        if not g:
            print("  %-8s n=0" % name)
            return
        print("  %-8s n=%4d  |vl| med %4d  pwm med %6d  ibat med %5d (min %5d max %5d)  vbat med %5d min %5d"
              % (name, len(g),
                 st.median([abs(r[1]) for r in g]),
                 st.median([abs(r[3]) for r in g]),
                 st.median([r[5] for r in g]), min(r[5] for r in g), max(r[5] for r in g),
                 st.median([r[6] for r in g]), min(r[6] for r in g)))

    print("\n=== group ===")
    s("moving", mov)
    s("mid", mid)
    s("stopped", stp)

    if mov and stp:
        im = st.median([r[5] for r in mov])
        isx = st.median([r[5] for r in stp])
        pm = st.median([abs(r[3]) for r in mov])
        ps = st.median([abs(r[3]) for r in stp])
        print("\n=== verdict ===")
        print("  pwm: moving %d vs stopped %d" % (pm, ps))
        print("  ibat: moving %d vs stopped %d mA" % (im, isx))
        if isx < max(60, im * 0.35):
            print("  => stopped frames draw almost NO current => power is CUT (driver/firmware), not a stall")
        elif isx > im * 0.7:
            print("  => stopped frames draw the SAME current => motor is powered but not turning"
                  " => either genuinely stalled OR the ENCODER is dropping counts")
        else:
            print("  => in between: current is lower but not zero")
        if ps > pm * 0.8:
            print("  => pwm stays high while stopped => the loop is saturated (railing)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
