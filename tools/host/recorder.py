"""CSV 记录 / 回放 / 统计。

CSV 里存的是协议原始整数字段（不做单位换算），所以录制 -> 回放 -> 再解析
与"直接从串口读"得到的数据完全一致，可以当黑匣子用。
"""

from __future__ import annotations

import csv
import os
import time
from dataclasses import dataclass

import protocol as P

CSV_FIELDS = ("host_ms",) + P.TELEM_FIELDS


class CsvRecorder:
    """把遥测逐帧写进 CSV。"""

    def __init__(self, path: str) -> None:
        self.path = path
        self.count = 0
        self.t0 = time.monotonic()
        directory = os.path.dirname(os.path.abspath(path))
        if directory:
            os.makedirs(directory, exist_ok=True)
        self._fh = open(path, "w", newline="", encoding="utf-8")
        self._w = csv.writer(self._fh)
        self._w.writerow(CSV_FIELDS)

    def write(self, t: P.Telemetry) -> None:
        if self._fh.closed:
            return
        row = [int((time.monotonic() - self.t0) * 1000)]
        row.extend(getattr(t, f) for f in P.TELEM_FIELDS)
        self._w.writerow(row)
        self.count += 1

    def flush(self) -> None:
        if not self._fh.closed:
            self._fh.flush()

    def close(self) -> None:
        if not self._fh.closed:
            self._fh.flush()
            self._fh.close()

    def __enter__(self) -> "CsvRecorder":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def read_csv(path: str) -> list[P.Telemetry]:
    """读回 CSV，返回遥测列表（按文件顺序）。"""
    out: list[P.Telemetry] = []
    with open(path, "r", newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        missing = [f for f in P.TELEM_FIELDS if f not in (reader.fieldnames or [])]
        if missing:
            raise ValueError("CSV 缺列: %s" % ", ".join(missing))
        for row in reader:
            out.append(P.Telemetry(**{f: int(row[f]) for f in P.TELEM_FIELDS}))
    return out


def count_rows(path: str) -> int:
    with open(path, "r", encoding="utf-8") as fh:
        return max(0, sum(1 for _ in fh) - 1)


@dataclass
class Summary:
    n: int = 0
    duration_s: float = 0.0
    rate_hz: float = 0.0
    yaw_min: float = 0.0
    yaw_max: float = 0.0
    v_max: float = 0.0
    vbat_min: float = 0.0
    vbat_max: float = 0.0
    ibat_max: float = 0.0
    stall_frames: int = 0
    lost_line_frames: int = 0
    marker_count: int = 0
    states: tuple = ()


def summarize(frames: list[P.Telemetry]) -> Summary:
    if not frames:
        return Summary()
    s = Summary(n=len(frames))
    s.duration_s = (frames[-1].t_ms - frames[0].t_ms) / 1000.0
    s.rate_hz = s.n / s.duration_s if s.duration_s > 0 else 0.0
    s.yaw_min = min(f.yaw for f in frames)
    s.yaw_max = max(f.yaw for f in frames)
    s.v_max = max(max(abs(f.v_l_mmps), abs(f.v_r_mmps)) for f in frames)
    s.vbat_min = min(f.vbat for f in frames)
    s.vbat_max = max(f.vbat for f in frames)
    s.ibat_max = max(f.ibat for f in frames)
    s.stall_frames = sum(1 for f in frames if f.flags & P.F_STUCK)
    s.lost_line_frames = sum(1 for f in frames if f.flags & P.F_LINE_LOST)
    # 黑标 = 整排压线
    prev = 0
    for f in frames:
        if f.line_mask == 0xFF and prev != 0xFF:
            s.marker_count += 1
        prev = f.line_mask
    s.states = tuple(sorted({f.state_id for f in frames}))
    return s
