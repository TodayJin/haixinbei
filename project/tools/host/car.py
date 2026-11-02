"""
**不依赖上位机的命令行工具**（用户建议：调试只用串口就行）。

为什么要有它：DAPLink（FAED:4870）是一颗芯片三个接口
  MI_00 = CMSIS-DAP(SWD) · MI_01 = HID · MI_02 = CDC(COM4)
上位机以 200Hz 从 COM4 连续收遥测，会把 DAPLink 固件喂满
-> 它的 **SWD 引擎被饿死/卡住**（实测：上位机跑过之后 openocd 一律
  `CMSIS-DAP command CMD_INFO failed`；停掉上位机也没用，要整机断电才恢复）。

所以：**要同时用 SWD 和串口时就别起上位机** —— 用这个脚本。
复用桥的协议实现（bridge.protocol / bridge.link），但**不起 HTTP 服务**。

用法：
  python A/_tools/car.py telem --sec 3
  python A/_tools/car.py drive wheels 2000 2000
  python A/_tools/car.py drive off
"""
from __future__ import annotations

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE = os.path.abspath(os.path.join(HERE, "..", "_haixinbei-push", "tools", "console"))
sys.path.insert(0, CONSOLE)

from bridge.link import SerialLink        # noqa: E402
from bridge import protocol as pr        # noqa: E402

MODES = {"off": 0, "wheels": 1, "base_turn": 2, "trace": 3}


def drain(lk: SerialLink, sec: float, want=None):
    """收 sec 秒的帧，返回列表。`poll(now_ms)` 直接返回这一批 Frame。"""
    out = []
    t0 = time.time()
    k = 0
    while time.time() - t0 < sec:
        k += 1
        try:
            frames = lk.poll(k)
        except Exception as e:
            print("  poll 出错：%s" % e)
            break
        for f in (frames or []):
            if want is None or f.type in want:
                out.append(f)
        time.sleep(0.003)
    return out


def cmd_telem(lk: SerialLink, sec: float) -> int:
    print("    t(ms)    vl    vr   pwm_l  pwm_r   vbat  ibat(mA)  flags  gray")
    t0 = time.time()
    last = -1.0
    n = 0
    end = time.time() + sec
    k = 0
    while time.time() < end:
        k += 1
        for f in (lk.poll(k) or []):
            if f.type != pr.T_TELEM2:
                continue
            t = pr.decode_telemetry(f.payload)
            n += 1
            el = time.time() - t0
            if el - last >= 0.2:
                last = el
                g = "".join(str(x) for x in (t.get("gray") or []))
                fl = t.get("flags")
                if isinstance(fl, dict):
                    fls = "".join(k[0].upper() for k, v in fl.items() if v) or "-"
                else:
                    fls = "0x%02x" % (fl or 0)
                print("  %7.0f %5d %5d %6d %6d %6d %8d  %-8s %s" % (
                    el * 1000, t.get("vl", 0), t.get("vr", 0),
                    t.get("pwm_l", 0), t.get("pwm_r", 0),
                    t.get("vbat_mv", 0), t.get("ibat_ma", 0),
                    fls, g[:10]))
        time.sleep(0.003)
    print("  （读到 %d 帧遥测）" % n)
    return 0 if n else 1


def cmd_spin(lk: SerialLink, mode: str, a: int, b: int, sec: float) -> int:
    """*决定性测试：发一个固定目标，同时把遥测打出来（全程不起上位机）。
    看 vl/vr 有没有跟着动 -> 编码器到底有没有在工作。"""
    print("  下发 drive(%s, %d, %d)，持续 %.1f 秒（每 200ms 重发一次喂看门狗）" % (mode, a, b, sec))
    print("    t(ms)    vl    vr   pwm_l  pwm_r   vbat  ibat(mA)  flags")
    t0 = time.time()
    last_send = 0.0
    last_print = -1.0
    n = 0
    k = 0
    vmax = 0
    while time.time() - t0 < sec:
        now = time.time() - t0
        if now - last_send >= 0.08:      # *80ms：200ms 会掉进看门狗，PWM 中途归零
            last_send = now
            lk.send(pr.frame_drive(20, MODES.get(mode, 1), a, b))
        k += 1
        for f in (lk.poll(k) or []):
            if f.type != pr.T_TELEM2:
                continue
            t = pr.decode_telemetry(f.payload)
            n += 1
            vmax = max(vmax, abs(t.get("vl", 0)), abs(t.get("vr", 0)))
            if now - last_print >= 0.2:
                last_print = now
                print("  %7.0f %5d %5d %6d %6d %6d %8d" % (
                    now * 1000, t.get("vl", 0), t.get("vr", 0),
                    t.get("pwm_l", 0), t.get("pwm_r", 0),
                    t.get("vbat_mv", 0), t.get("ibat_ma", 0)))
        time.sleep(0.003)
    lk.send(pr.frame_drive(21, 0, 0, 0))
    print("  （%d 帧；|vl|/|vr| 峰值 = %d）" % (n, vmax))
    if vmax < 5:
        print("  [BAD] 全程几乎为 0 -> **编码器确实没有输出**（在 PWM 已经很高的前提下）")
    else:
        print("  [OK] 有读数 -> 编码器是好的")
    return 0


def cmd_stair(lk: SerialLink, targets, sec: float, mode: str) -> int:
    """*一次量完整条传递曲线（全程不起上位机）。
    每档跑 sec 秒，期间每 80ms 重发驾驶帧喂看门狗；取**后半段**当稳态。"""
    MM = None
    rows = []
    print("  模式=%s  每档 %.1fs" % (mode, sec))
    print("   目标 | 期望计数 | 实测计数 |  mm/s |  PWM  | 比值 | 抖动 | 电池")
    for T in targets:
        want = None
        mark = len(rows)
        t0 = time.time()
        last_send = 0.0
        k = 0
        while time.time() - t0 < sec:
            now = time.time() - t0
            if now - last_send >= 0.08:
                last_send = now
                lk.send(pr.frame_drive(30, MODES.get(mode, 1), T if mode != "base_turn" else 0,
                                       T if mode != "base_turn" else T))
            k += 1
            for f in (lk.poll(k) or []):
                if f.type != pr.T_TELEM2:
                    continue
                d = pr.decode_telemetry(f.payload)
                rows.append({"i": len(rows), "ms": now, "vl": d.get("vl", 0), "vr": d.get("vr", 0),
                             "pl": d.get("pwm_l", 0), "pr": d.get("pwm_r", 0),
                             "vb": d.get("vbat_mv", 0)})
            time.sleep(0.002)
        # 取后半段
        seg = [r for r in rows[mark:] if r["ms"] >= sec * 0.4]
        if not seg:
            print("  %5d | （没采到帧）" % T)
            continue
        a = [abs(r["vl"]) for r in seg]
        p = [abs(r["pl"]) for r in seg]
        mean = lambda x: sum(x) / len(x)
        sd = lambda x: (sum((v - mean(x)) ** 2 for v in x) / len(x)) ** 0.5
        vl = mean(a)
        c = vl / 28.05 if MM is None else vl / MM      # cpr=1456,dia=65 -> 1 计数/5ms = 28.05 mm/s
        wantc = T * 45.0 / 7200.0
        print("  %5d | %8.2f | %8.2f | %4.0f | %5.0f | %5.3f | %4.0f | %5.0f" % (
            T, wantc, c, vl, mean(p), c / wantc, sd(a), mean([r["vb"] for r in seg])))
    lk.send(pr.frame_drive(31, 0, 0, 0))
    return 0


# 常用参数 id（与 host_link.h 一致）
PARAM_IDS = {"gyro_kp": 0x0101, "gyro_ki": 0x0102, "gyro_kd": 0x0103, "gyro_ilimit": 0x0104, "gyro_ang_kp": 0x010A, "gyro_rate_kp": 0x010B, "gyro_rate_kd": 0x010C, "gyro_rate_max": 0x010D, "gyro_corr_limit": 0x010E, "gyro_corr_slew": 0x010F, "gyro_settle_deg": 0x0110, "gyro_settle_ms": 0x0111, "gyro_corr_ref": 0x0112, "trace_rate_max": 0x020A, 
    "counts_per_rev": 0x0305, "wheel_dia": 0x0306, "wheel_base": 0x0307,
    "speed_full_counts": 0x0308, "speed_kp": 0x0601, "speed_ki": 0x0602,
    "speed_kd": 0x0603, "speed_ilimit": 0x0604, "speed_ff_slope": 0x0605,
    "speed_ff_fric": 0x0606, "line_kp": 0x0201, "line_ki": 0x0202, "line_kd": 0x0203,
}


def cmd_set(lk: SerialLink, pairs, save: bool) -> int:
    """*直接写参数到车（不经上位机）。pairs = ["speed_full_counts=13", ...]"""
    for i, s in enumerate(pairs):
        if "=" not in s:
            print("  格式应为 key=value：%s" % s)
            continue
        k, v = s.split("=", 1)
        pid = PARAM_IDS.get(k.strip())
        if pid is None:
            print("  不认识的参数名：%s" % k)
            continue
        val = float(v)
        lk.send(pr.frame_param_set(100 + i, pid, val))
        print("  已下发 %s(0x%04X) = %s" % (k.strip(), pid, val))
        # 等 ACK
        t0 = time.time()
        while time.time() - t0 < 0.4:
            for f in (lk.poll(1) or []):
                if f.type == pr.T_ACK:
                    try:
                        print("     ACK %s" % pr.decode_ack(f.payload))
                    except Exception:
                        pass
            time.sleep(0.01)
    if save:
        lk.send(pr.frame_param_save(199))
        print("  已发固化(T_PARAM_SAVE)")
        time.sleep(0.5)
        lk.send(pr.frame_param_get(200))
        got = {}
        t0 = time.time()
        while time.time() - t0 < 2.0:
            for f in (lk.poll(1) or []):
                if f.type == pr.T_PARAM_REPORT:
                    try:
                        pid, val = pr.decode_param_report(f.payload)
                        got[pid] = val
                    except Exception:
                        pass
            time.sleep(0.01)
        for k2, v2 in sorted(PARAM_IDS.items(), key=lambda x: x[1]):
            if v2 in got:
                print("    回读 %-20s = %s" % (k2, got[v2]))
    return 0


def cmd_text(lk: SerialLink, sec: float) -> int:
    """收 sec 秒，把固件发来的文本帧（T_TEXT）打出来。
    用途：读 [fw] stops low/wd/off/cmd —— 谁停了驾驶会话的诊断计数（不需要 SWD）。"""
    n = 0
    t0 = time.time()
    k = 0
    while time.time() - t0 < sec:
        k += 1
        for f in (lk.poll(k) or []):
            if f.type == pr.T_TEXT:
                try:
                    s = pr.decode_text(f.payload)
                except Exception:
                    s = f.payload.hex()
                n += 1
                print("  " + s.rstrip())
        time.sleep(0.003)
    print("  (%d 条文本)" % n)
    return 0 if n else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["telem", "drive", "spin", "stair", "set", "text"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", default=os.environ.get("CAR_PORT", "COM4"))
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--sec", type=float, default=1.5)
    a = ap.parse_args()

    lk = SerialLink(port=a.port, baud=a.baud)
    lk.start()
    rc = 0
    try:
        time.sleep(1.0)                       # 等首批遥测
        if a.cmd == "text":
            rc = cmd_text(lk, a.sec)
        elif a.cmd == "set":
            rc = cmd_set(lk, a.args, True)
        elif a.cmd == "telem":
            rc = cmd_telem(lk, a.sec)
        elif a.cmd == "stair":
            mode = a.args[0] if a.args else "wheels"
            ts = [int(x) for x in (a.args[1].split(",") if len(a.args) > 1 else ["1500", "3000", "4500", "6000"])]
            rc = cmd_stair(lk, ts, a.sec, mode)
        elif a.cmd == "spin":
            mode = a.args[0] if a.args else "wheels"
            x = int(a.args[1]) if len(a.args) > 1 else 3000
            y = int(a.args[2]) if len(a.args) > 2 else x
            rc = cmd_spin(lk, mode, x, y, a.sec)
        else:
            mode = a.args[0] if a.args else "off"
            x = int(a.args[1]) if len(a.args) > 1 else 0
            y = int(a.args[2]) if len(a.args) > 2 else 0
            nb = lk.send(pr.frame_drive(9, MODES.get(mode, 0), x, y))
            print("  drive(%s, %d, %d) 发出 %d 字节" % (mode, x, y, nb))
            for f in drain(lk, 0.5, want=(pr.T_ACK, pr.T_DRIVE_REPORT)):
                print("    回包 type=0x%02X payload=%s" % (f.type, f.payload.hex()))
    finally:
        try:
            lk.send(pr.frame_drive(10, 0, 0, 0))   # 无论如何停车
        except Exception:
            pass
        lk.stop()
    return rc


if __name__ == "__main__":
    sys.exit(main())
