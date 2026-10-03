"""钉住 bridge.bench 的判定/汇总口径（纯函数，不启桥、不碰串口）。

为什么值得钉：`bridge.bench` 输出的数字就是要写进报告、写进文档的实测值
（帧率 / 间隔分位 / 丢帧 / CPU / 串口分诊）。口径漂了没人看得出来，所以这里用
合成事件把它按死：

- `parse_sse` 必须扛得住 TCP 任意切分（半条报文留到下一次）与 `: ping` 心跳；
- `summarize` 的 fps / 间隔分位 / 丢帧 / 字节率 / 事件构成都要能算对，空输入不能炸；
- `format_report` 里必须真的出现那些数（不然"量了"等于没量）；
- `serial_verdict` 四种分诊（出帧 / 没连上 / 有字节解不出 / 一字节没有）各判各的。
"""
from __future__ import annotations

import json

from bridge import bench


def _ev(name: str, t: float, nbytes: int = 100, payload=None):
    return (name, payload if payload is not None else {}, t, nbytes)


# ------------------------------------------------------------------ parse_sse
def test_parse_sse_splits_complete_and_keeps_partial():
    buf = (b"event: telem\ndata: {\"a\": 1}\n\n"
           b": ping\n\n"
           b"event: stats\ndata: {\"b\": 2}\n\n"
           b"event: telem\ndata: {\"c\"")
    evs, rest = bench.parse_sse(buf)
    assert [e[0] for e in evs] == ["telem", "stats"]
    assert evs[0][1] == {"a": 1}
    assert evs[1][1] == {"b": 2}
    assert rest == b"event: telem\ndata: {\"c\""          # 半条原样留着
    # 报文字节数 = raw + 2（那两个 \n）
    assert evs[0][2] == len(b"event: telem\ndata: {\"a\": 1}") + 2


def test_parse_sse_resumes_after_partial():
    first, rest = bench.parse_sse(b"event: telem\ndata: {\"x\": 7}\n\nevent: tel")
    assert len(first) == 1 and first[0][1] == {"x": 7}
    second, rest2 = bench.parse_sse(rest + b"em\ndata: {\"y\": 8}\n\n")
    assert len(second) == 1 and second[0][1] == {"y": 8}
    assert rest2 == b""


def test_parse_sse_drops_broken_json_and_nameless_events():
    evs, rest = bench.parse_sse(b"event: telem\ndata: {oops\n\ndata: {\"k\": 1}\n\n")
    assert len(evs) == 1
    assert evs[0][0] is None                              # 没有 event 行：名字回 None
    assert evs[0][1] == {"k": 1}
    assert rest == b""


# ----------------------------------------------------------------------- pct
def test_pct_endpoints_and_empty():
    assert bench.pct([], 50) == 0.0
    assert bench.pct([5], 95) == 5
    assert bench.pct([1, 2, 3, 4], 0) == 1
    assert bench.pct([1, 2, 3, 4], 100) == 4
    assert bench.pct([1, 2, 3, 4], 50) == 3               # 最近秩，不插值


# ----------------------------------------------------------------- summarize
def test_summarize_counts_rates_and_gaps():
    evs = [_ev("telem", 0.00), _ev("telem", 0.01), _ev("telem", 0.02),
           _ev("telem", 0.03), _ev("stats", 0.035, 40), _ev("log", 0.04, 60)]
    rep = bench.summarize(evs, dt=1.0, fed_frames=6, cpu=12.0,
                          log="C:/tmp/bench_bridge_x.log")
    assert rep["telem"] == 4
    assert rep["fps"] == 4.0
    assert rep["fed"] == 6 and rep["dropped"] == 2 and rep["fed_fps"] == 6.0
    assert rep["gap_p50"] == 10.0 and rep["gap_p95"] == 10.0 and rep["gap_max"] == 10.0
    assert rep["events"] == 6 and rep["ev_per_s"] == 6.0
    assert rep["bytes"] == 100 * 4 + 40 + 60
    assert rep["kinds"] == {"telem": 4, "stats": 1, "log": 1}
    assert rep["cpu"] == 12.0


def test_summarize_empty_input_does_not_divide_by_zero():
    rep = bench.summarize([], dt=0.0, fed_frames=7)
    assert rep["fps"] == 0.0 and rep["telem"] == 0
    assert rep["gap_p50"] == 0.0 and rep["gap_max"] == 0.0
    assert rep["dropped"] == 7                            # 喂了 7 帧、一条没到
    assert rep["fed_fps"] is None                         # dt=0：无从报速率
    assert rep["cpu"] is None

def test_summarize_without_fed_count_reports_none_not_zero():
    rep = bench.summarize([_ev("telem", 0.0)], dt=1.0)
    assert rep["fed"] is None and rep["dropped"] is None and rep["fed_fps"] is None


# --------------------------------------------------------------------- expand
def test_expand_unpacks_telem_batch_and_splits_bytes():
    evs = [("telem_batch", [{"i": 1}, {"i": 2}], 1.0, 200), ("telem", {"i": 3}, 2.0, 90)]
    out = bench.expand(evs)
    assert [o[0] for o in out] == [{"i": 1}, {"i": 2}, {"i": 3}]
    assert [o[2] for o in out] == [100, 100, 90]          # 报文字节按帧数摊
    assert bench.expand([_ev("stats", 0.0)]) == []        # 非遥测事件不算帧


def test_summarize_counts_frames_inside_batches_not_messages():
    evs = [("telem_batch", [{"i": 1}, {"i": 2}, {"i": 3}], 0.00, 300),
           ("telem_batch", [{"i": 4}, {"i": 5}, {"i": 6}], 0.01, 300)]
    rep = bench.summarize(evs, dt=1.0, fed_frames=6)
    assert rep["telem"] == 6 and rep["fps"] == 6.0        # 合并了也要报 6 帧/s，不是 2
    assert rep["events"] == 2 and rep["ev_per_s"] == 2.0
    assert rep["batched"] == 2 and rep["per_event"] == 3.0
    assert rep["dropped"] == 0
    assert rep["kinds"] == {"telem_batch": 2}


def test_format_report_adds_batch_line_only_when_batched():
    evs = [("telem_batch", [{"i": 1}, {"i": 2}], 0.00, 200),
           ("telem_batch", [{"i": 3}, {"i": 4}], 0.01, 200)]
    lines = bench.format_report(bench.summarize(evs, dt=1.0, fed_frames=4), "回放 100Hz")
    assert len(lines) == 4 and "telem_batch" in lines[3] and "2 条" in lines[3]
    plain = bench.summarize([_ev("telem", 0.0), _ev("telem", 0.01)], dt=1.0)
    assert len(bench.format_report(plain, "回放 50Hz")) == 3   # 没合并就不加那行


# --------------------------------------------------------------- format_report
def test_format_report_shows_the_measured_numbers():
    rep = bench.summarize([_ev("telem", 0.00), _ev("telem", 0.01), _ev("telem", 0.02)],
                          dt=0.5, fed_frames=3, cpu=2.0, log="C:/x/bench_sim.log")
    lines = bench.format_report(rep, "仿真车 100Hz")
    assert len(lines) == 3
    joined = "\n".join(lines)
    assert "仿真车 100Hz" in joined
    assert "6.0 帧/s" in joined                           # 3 帧 / 0.5s
    assert "p50  10.0ms" in joined
    assert "2.0%" in joined and "bench_sim.log" in joined
    assert "丢 0 帧" in joined


# -------------------------------------------------------------- serial_verdict
def test_serial_verdict_frames_flowing():
    rc, why = bench.serial_verdict({"connected": True, "stats": {"rx": 900, "tx": 3,
                                                                 "crc": 0, "resync": 0}}, 120)
    assert rc == 0 and "链路在出帧" in why


def test_serial_verdict_not_connected():
    rc, why = bench.serial_verdict({"connected": False, "stats": {"rx": 0}}, 0)
    assert rc == 2 and "没连上串口" in why


def test_serial_verdict_bytes_but_no_frames_points_at_baud():
    rc, why = bench.serial_verdict({"connected": True, "port": "COM26",
                                    "stats": {"rx": 1423, "tx": 0}}, 0)
    assert rc == 2 and "波特率不匹配" in why and "bridge.probe" in why and "COM26" in why


def test_serial_verdict_silent_line_points_at_wiring_and_baud():
    rc, why = bench.serial_verdict({"connected": True, "stats": {"rx": 0}}, 0)
    assert rc == 2 and "一个字节都没收到" in why and "bridge.probe" in why


# --------------------------------------------------------------- format helpers
def test_format_serial_reports_counters_and_firmware():
    payload = {"port": "COM26", "baud": 230400, "connected": True, "telem_hz": 100,
               "fw": "1.2.3", "stats": {"rx": 10, "tx": 2, "crc": 0, "resync": 0, "drops": 1}}
    lines = bench.format_serial({"link": payload, "why": "链路在出帧"})
    joined = "\n".join(lines)
    assert "COM26 @230400" in joined and "rx=10" in joined and "drops=1" in joined
    assert "1.2.3" in joined


def test_format_sim_reports_drive_latency():
    sim = {"rtt": [16.0, 32.0], "drive_seen": [17.0, 33.0], "rtt_p50": 16.0,
           "drive_p50": 17.0, "link": {"stats": {"rx": 528}}, "log": "C:/x/bench_sim.log"}
    lines = bench.format_sim(sim)
    joined = "\n".join(lines)
    assert "17ms" in joined and "33ms" in joined
    assert json.loads(lines[2].split("链路口径 ")[1].split(" | ")[0]) == {"rx": 528}
