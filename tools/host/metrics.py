"""调参用的量化指标（纯函数，可单测）。

调 PID 只看波形容易糊弄自己：把"晃了几下""大概多久稳住"变成数字，
改参数前后才有得比。
"""

from __future__ import annotations

import math


def mean(ys) -> float:
    return sum(ys) / len(ys) if ys else 0.0


def stdev(ys) -> float:
    if len(ys) < 2:
        return 0.0
    m = mean(ys)
    return math.sqrt(sum((y - m) ** 2 for y in ys) / (len(ys) - 1))


def rms(ys) -> float:
    return math.sqrt(sum(y * y for y in ys) / len(ys)) if ys else 0.0


def zero_crossings(ys, deadband: float = 0.0) -> int:
    """过零次数（振荡次数的两倍）。deadband 用来忽略噪声抖动。"""
    n = 0
    sign = 0
    for y in ys:
        s = 1 if y > deadband else (-1 if y < -deadband else 0)
        if s and sign and s != sign:
            n += 1
        if s:
            sign = s
    return n


def overshoot(ys, target: float = 0.0) -> float:
    """最大超调量（相对 target 的绝对值），正数。"""
    if not ys:
        return 0.0
    if target == 0.0:
        return max(abs(y) for y in ys)
    peak = max(ys) if target >= 0 else min(ys)
    return max(0.0, abs(peak - target) - 0.0)


def settling_time(xs, ys, target: float, band: float) -> float | None:
    """稳定时间：从起点到"最后一次离开 target±band 之后那一点"的时长（秒）。

    一直没进入过（结尾还在带外）返回 None；一开始就在带内返回 0。
    """
    if not xs or len(xs) != len(ys):
        return None
    last_out = None
    for i, y in enumerate(ys):
        if abs(y - target) > band:
            last_out = i
    if last_out is None:
        return 0.0
    if last_out >= len(xs) - 1:
        return None
    return xs[last_out + 1] - xs[0]


def summarize(ys, target: float = 0.0, deadband: float = 0.0) -> dict:
    """一组样本的常用指标。空数据返回全 0，界面直接显示不会炸。"""
    if not ys:
        return {"n": 0, "min": 0.0, "max": 0.0, "mean": 0.0, "std": 0.0,
                "pp": 0.0, "rms": 0.0, "zc": 0, "overshoot": 0.0}
    return {
        "n": len(ys),
        "min": min(ys),
        "max": max(ys),
        "mean": mean(ys),
        "std": stdev(ys),
        "pp": max(ys) - min(ys),
        "rms": rms(ys),
        "zc": zero_crossings(ys, deadband),
        "overshoot": overshoot(ys, target),
    }


def format_metrics(s: dict, unit: str = "") -> str:
    if not s["n"]:
        return "—"
    u = (" " + unit) if unit else ""
    return ("n=%d  峰峰 %.3g%s  均 %.3g  标准差 %.3g  过零 %d 次"
            % (s["n"], s["pp"], u, s["mean"], s["std"], s["zc"]))
