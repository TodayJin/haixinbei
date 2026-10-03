"""钉住桥侧 SSE 攒批（`--sse-batch-ms`）的口径。

为什么要钉：攒批把「一条事件一条报文」换成「连续遥测合成一条 `telem_batch`」——
一边是报文数与浏览器唤醒数减半（"消除卡顿"的另一半），另一边是**任何按报文数算帧率
的东西都会把帧率报成一半**。所以这里钉两件事：

- `group_sse` 的纯函数口径：只有**连续**遥测合并、单帧仍发 `telem`（形状不变）、
  非遥测事件原样穿过且保序、`batch_max` 只切分不丢帧；
- 真 HTTP 流上：开着攒批确实收到 `telem_batch`、`--sse-batch-ms 0` 确实一条都不发，
  并用 `bridge.bench` 自己的 SSE 解析器数帧（帧数按载荷里的帧算，不按报文数算）。
"""
from __future__ import annotations

import os
import threading
import time

import pytest

from bridge import server
from bridge.bench import Sse, expand

CONSOLE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _ev(name: str, payload=None) -> dict:
    return {"n": name, "src": {"x": 1} if payload is None else payload}


# ------------------------------------------------------------------- group_sse
def test_group_sse_merges_consecutive_telem():
    out = server.group_sse([_ev("telem", {"i": 1}), _ev("telem", {"i": 2}),
                            _ev("telem", {"i": 3})])
    assert out == [("telem_batch", [{"i": 1}, {"i": 2}, {"i": 3}])]


def test_group_sse_single_telem_keeps_old_shape():
    out = server.group_sse([_ev("telem", {"i": 1})])
    assert out == [("telem", {"i": 1})]                 # 单帧不让前端换形状


def test_group_sse_passes_other_events_through_in_order():
    out = server.group_sse([_ev("telem", {"i": 1}), _ev("log", {"text": "hi"}),
                            _ev("telem", {"i": 2}), _ev("telem", {"i": 3}),
                            _ev("stats", {"rx": 9})])
    assert out == [("telem", {"i": 1}), ("log", {"text": "hi"}),
                   ("telem_batch", [{"i": 2}, {"i": 3}]), ("stats", {"rx": 9})]


def test_group_sse_batch_max_splits_without_dropping():
    evs = [_ev("telem", {"i": i}) for i in range(5)]
    out = server.group_sse(evs, batch_max=2)
    assert [n for n, _p in out] == ["telem_batch", "telem_batch", "telem"]
    assert sum(len(p) if isinstance(p, list) else 1 for _n, p in out) == 5


def test_group_sse_unnamed_event_falls_back_to_msg():
    out = server.group_sse([{"src": {"a": 1}}])
    assert out == [("msg", {"a": 1})]


# ------------------------------------------------------- 真 HTTP 流（自带桥）
@pytest.fixture
def mk_bridge(tmp_path):
    """按需要起独立的小桥（自带端口 0），跑完关掉 —— 不动 session 级的 `live`。"""
    made: list = []

    def _make(batch_ms: int):
        b = server.Bridge(mode="sim", console_dir=CONSOLE, sse_batch_ms=batch_ms,
                          preset_dir=os.path.join(str(tmp_path), "presets"),
                          log_dir=os.path.join(str(tmp_path), "logs"))
        b.start()
        httpd = server.serve(b, "127.0.0.1", 0)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        made.append((httpd, b))
        return httpd.server_address[1], b

    yield _make
    for httpd, b in made:
        httpd.shutdown()
        b.stop()


def _spam(console, n: int, hz: int, log_at: int | None = None):
    """直接往 console 灌 n 帧遥测（不靠仿真车的帧率，那些由 sim 的时钟决定）。

    `log_at` 非 None 时在第 log_at 帧后插一条 log 事件，用来验非遥测事件不被吞。
    """
    def run():
        for i in range(n):
            if log_at is not None and i == log_at:
                console.emit({"n": "log", "src": {"text": "hi"}})
            console.emit({"n": "telem", "src": {"i": i}})
            time.sleep(1.0 / hz)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def _kinds(evs) -> dict:
    out: dict[str, int] = {}
    for n, _o, _t, _nb in evs:
        out[n] = out.get(n, 0) + 1
    return out


def test_stream_batches_telemetry_when_enabled(mk_bridge):
    port, b = mk_bridge(20)                             # 20ms 一攒：200Hz 下每条约 4 帧
    s = Sse(port)
    try:
        s.pump(0.5)
        th = _spam(b.console, 200, 200, log_at=50)
        evs = s.pump(1.5)
    finally:
        s.close()
    th.join(timeout=3)
    kinds = _kinds(evs)
    assert kinds.get("telem_batch", 0) >= 5, kinds       # 真的攒了，不是一条一条发
    assert len(expand(evs)) >= 195, kinds                # 攒批不许丢帧
    assert kinds.get("log", 0) == 1, kinds               # 非遥测事件照样穿过去
    for n, payload, _t, _nb in evs:
        if n == "telem_batch":
            assert isinstance(payload, list) and len(payload) >= 2


def test_stream_without_batching_sends_single_events(mk_bridge):
    port, b = mk_bridge(0)                              # 0 = 老行为
    s = Sse(port)
    try:
        s.pump(0.5)
        th = _spam(b.console, 120, 120)
        evs = s.pump(1.5)
    finally:
        s.close()
    th.join(timeout=3)
    kinds = _kinds(evs)
    assert kinds.get("telem_batch", 0) == 0, kinds       # 一条 telem_batch 都不许有
    assert kinds.get("telem", 0) >= 115, kinds
