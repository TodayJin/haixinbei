"""★航位推算轨迹（RouteTracker）：编码器 mm/s + IMU yaw 积分。

用户 2026-10-11："通过编码器以及 imu 的数据在上位机上给我一个行进路线的缩略图"
—— 固件一行没改，靠遥测里本来就有的 `vl/vr`（mm/s）和 `yaw`（度）。

这个数学的坑很隐蔽（单位、dt、断流），所以逐条钉住。
"""
from __future__ import annotations

import math

from bridge.state import ROUTE_MAX_DT_MS, ROUTE_MIN_STEP_MM, RouteTracker


def _frames(r: RouteTracker, n: int, *, v: float = 20.0, yaw: float = 0.0,
            t0: int = 1000, step: int = 5, lost: bool = False, state: int = 1) -> int:
    """喂 n 帧，每帧间隔 step ms。返回下一帧的固件时间戳。"""
    t = t0
    for _ in range(n):
        r.feed({"t": t, "vl": v, "vr": v, "yaw": yaw, "state_id": state,
                "flags": {"lost": lost}})
        t += step
    return t


def test_straight_line_uses_mm_per_second():
    """直行：20 mm/s × 1 秒 = 20 mm（单位写错这里立刻红）。

    ★注意**第一帧只定时间基准、不积分**（没有 dt 可言）⇒ N 帧只有 N-1 个间隔，
      所以 200 帧 × 5ms 是 0.995 s = 19.9 mm。这不是 bug，是必须有的行为。
    """
    r = RouteTracker()
    _frames(r, 200, v=20.0, yaw=0.0)          # 200 帧 = 199 个 5ms 间隔 = 0.995 s
    assert math.isclose(r.x, 19.9, abs_tol=0.05), r.x
    assert math.isclose(r.y, 0.0, abs_tol=0.05), r.y
    assert math.isclose(r.dist_mm, 19.9, abs_tol=0.05), r.dist_mm
    # 再来 200 帧 ⇒ 恰好再多 199 个间隔
    _frames(r, 200, v=20.0, yaw=0.0, t0=5000)
    assert math.isclose(r.x, 39.8, abs_tol=0.06), r.x


def test_yaw_is_degrees_and_turns_the_path():
    """yaw 是**度**：90° 应该往 +y 走，不是往 +x。"""
    r = RouteTracker()
    _frames(r, 200, v=20.0, yaw=90.0)          # 同上：0.995 s
    assert abs(r.x) < 0.05, r.x
    assert math.isclose(r.y, 19.9, abs_tol=0.05), r.y


def test_gap_is_not_bridged():
    """★断流（dt 过大）时**绝不能跨过去画一条直线**：位置一点也不许动。"""
    r = RouteTracker()
    t = _frames(r, 20, v=20.0)
    x0, y0, d0 = r.x, r.y, r.dist_mm
    # 断流 1 秒（远超 ROUTE_MAX_DT_MS=200）——车可能在这期间被挪走了，但我们不知道
    r.feed({"t": t + 1000, "vl": 20.0, "vr": 20.0, "yaw": 0.0, "state_id": 1, "flags": {}})
    assert r.x == x0 and r.y == y0 and r.dist_mm == d0


def test_stopped_car_does_not_drift_or_grow():
    """★停车（v=0）时积分严格为 0 ⇒ 既不漂、也不涨点（按距离抽稀的意义）。"""
    r = RouteTracker()
    _frames(r, 100, v=20.0)                    # 先走一段
    pts, d = len(r.pts), r.dist_mm
    x0, y0 = r.x, r.y
    _frames(r, 400, v=0.0, t0=5000)            # 停 2 秒
    assert (r.x, r.y) == (x0, y0)
    assert r.dist_mm == d
    assert len(r.pts) == pts


def test_decimation_by_distance_not_time():
    """挪不到 ROUTE_MIN_STEP_MM 就不记点（按时间抽的话这里会涨一堆）。"""
    r = RouteTracker()
    # 每帧 5ms、速度 20mm/s ⇒ 每帧 0.1mm，远小于 8mm 阈值
    _frames(r, 10, v=20.0)
    assert len(r.pts) == 1                      # 只有起点
    _frames(r, 100, v=20.0, t0=2000)            # 再走 0.5s = 10mm ⇒ 该记点了
    assert len(r.pts) >= 2


def test_take_is_incremental_and_reset_is_flagged():
    """take() 只给增量（前端按 from 拼接），reset 之后要带 reset 标记。"""
    r = RouteTracker()
    _frames(r, 200, v=50.0)                     # 50mm/s × 1s = 50mm ⇒ 抽稀后 6~7 个点
    first = r.take()
    assert first is not None and first["reset"] is True
    assert first["from"] == 0 and len(first["pts"]) == first["n"]
    assert r.take() is None, "没有新东西就不该推（5Hz 节流时大多数调用都是 None）"
    _frames(r, 200, v=50.0, t0=9000)
    second = r.take()
    assert second is not None and "reset" not in second
    assert second["from"] == first["n"], "增量起点必须接着上次"
    assert second["n"] > first["n"]
    r.reset()
    assert r.take()["reset"] is True


def test_state_change_and_lost_make_marks():
    """打点：状态切换 / 丢线各记一个，带当时的坐标。"""
    r = RouteTracker()
    _frames(r, 40, v=20.0, state=1, t0=1000)
    _frames(r, 40, v=20.0, state=2, t0=1400)    # 切状态
    _frames(r, 40, v=20.0, state=2, t0=1800, lost=True)   # 丢线
    _frames(r, 40, v=20.0, state=2, t0=2200, lost=True)   # 一直丢着（只记一次）
    ks = [m["k"] for m in r.marks]
    assert ks.count("state") == 1 and ks.count("lost") == 1
    st = [m for m in r.marks if m["k"] == "state"][0]
    assert st["v"] == 2 and isinstance(st["x"], float)


def test_bad_frame_is_ignored_not_crashing():
    """缺字段 / 垃圾值不许把桥弄崩（遥测来自串口，什么都可能进来）。"""
    r = RouteTracker()
    for bad in ({}, {"t": 1}, {"t": 2, "vl": None, "vr": "x", "yaw": 0},
                {"t": 3, "vl": 20, "vr": 20, "yaw": float("nan")}):
        r.feed(bad)                             # 不抛就算过
    assert r.take() is not None


def test_u32_timestamp_wrap_does_not_explode():
    """固件的 t 是 u32，会绕回；按位减要对（不然会算出个天文数字的 dt）。"""
    r = RouteTracker()
    r.feed({"t": 0xFFFFFF00, "vl": 20.0, "vr": 20.0, "yaw": 0.0, "state_id": 1, "flags": {}})
    r.feed({"t": 0x00000010, "vl": 20.0, "vr": 20.0, "yaw": 0.0, "state_id": 1, "flags": {}})
    # 真实间隔 = 0x110 = 272ms > ROUTE_MAX_DT_MS ⇒ 按"断流"处理，不动
    assert r.x == 0.0
    assert ROUTE_MAX_DT_MS < 272
    assert ROUTE_MIN_STEP_MM > 0
