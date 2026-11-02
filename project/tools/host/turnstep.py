"""角度环阶跃响应测试（两段式）—— v2：修掉"标记错位"的 bug。

v1 的 bug：数据行是**先攒在内存、循环结束后才写文件**，而阶跃标记是在循环中写的
          ⇒ 标记落在第 1 行 ✗ ⇒ 分析脚本找不到它 ⇒ 数据全废 ✗
v2：**数据行随到随写**，标记写在它真正发生的位置 ✓

两段式（排除起步污染）：
  第 1 段 settle：base_turn 且 b=0 ⇒ 角度环先把当前航向稳住（环闭环、残留清干净）
  第 2 段 step  ：b 阶跃到目标角度
输出：CSV 到 pid_logs/turn2_*.csv（列 t_ms,yaw,err,rate_ref,corr,vl,vr,pwm_l,pwm_r；标记行首列为 -1）
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console")))
from bridge.link import SerialLink     # noqa: E402
from bridge import protocol as pr     # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--a", type=int, default=0)
    ap.add_argument("--angle", type=float, default=90.0)
    ap.add_argument("--settle", type=float, default=1.2)
    ap.add_argument("--sec", type=float, default=2.5)
    ap.add_argument("--keepalive", type=float, default=0.03)
    args = ap.parse_args()

    outdir = os.path.join(HERE, "pid_logs")
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "turn2_%s.csv" % time.strftime("%Y%m%d%H%M%S"))

    lk = SerialLink(port=args.port, baud=921600)
    lk.start()
    time.sleep(0.7)
    seq = 9000
    k = 0
    n_rows = 0
    last = -1e9
    stepped = False
    t_start = time.time()
    with open(path, "w", newline="", encoding="utf-8") as fh:
        wr = csv.writer(fh)
        wr.writerow(["t_ms", "yaw", "err", "rate_ref", "corr", "vl", "vr", "pwm_l", "pwm_r"])
        while True:
            el = time.time() - t_start
            if el > args.settle + args.sec:
                break
            b = 0 if el < args.settle else int(args.angle)
            now = time.time()
            if (now - last) >= args.keepalive:
                last = now
                seq += 1
                lk.send(pr.frame_drive(seq, 2, int(args.a), b))
                if (not stepped) and el >= args.settle:
                    stepped = True
                    wr.writerow([-1, -1, -1, -1, -1, -1, -1, -1, -1])   # 阶跃标记（真正的位置）
                    fh.flush()
            k += 1
            for f in (lk.poll(k) or []):
                if f.type != pr.T_TELEM2:
                    continue
                d = pr.decode_telemetry(f.payload)
                an = d.get("angle") or {}
                wr.writerow([d.get("t", 0), d.get("yaw", 0.0), an.get("err", 0.0),
                             an.get("rate_ref", 0.0), an.get("corr", 0.0),
                             d.get("vl", 0), d.get("vr", 0), d.get("pwm_l", 0), d.get("pwm_r", 0)])
                n_rows += 1
            time.sleep(0.002)
        seq += 1
        lk.send(pr.frame_drive(seq, 0, 0, 0))
    lk.stop()
    print("saved: %s  (%d frames, marker in place)" % (os.path.basename(path), n_rows))
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
