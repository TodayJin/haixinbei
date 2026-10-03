"""回放链路（`--replay`）的行为测试：最容易坏的是"绕回开头"那一步。

真实录制里 `t_host` 是**绝对**毫秒（1.7e12 量级）。绕回开头时如果只把 `t0` 设成
`now`，第二遍就得等 `t_host[0]/speed` 那么久才出帧 —— 表现是前端波形放完一遍就
再也不动了。这里用手工 jsonl 把这个行为钉死。
"""
from __future__ import annotations

import json
import time

import pytest

from bridge.link import LinkError, ReplayLink


def _write(path, count: int = 5, step_ms: int = 20) -> str:
    base = 1_700_000_000_000                     # 和真实录制一样的绝对毫秒
    with open(path, "w", encoding="utf-8") as fh:
        for i in range(count):
            fh.write(json.dumps({"n": "telem", "src": {"t": i}, "t_host": base + i * step_ms},
                                ensure_ascii=False) + "\n")
    return str(path)


def test_replay_link_loops(tmp_path):
    rl = ReplayLink(_write(tmp_path / "rec.jsonl"), speed=1.0)
    rl.start()
    assert len(rl.rows) == 5 and rl.note.startswith("回放")

    got = 0
    t0 = time.time()
    while time.time() - t0 < 1.0:                 # 5 行只占 100 ms，一秒里应该绕十几圈
        got += len(rl.poll(time.time() * 1000.0))
        time.sleep(0.005)
    assert got >= 20, f"绕圈之后帧太少（{got}）——多半是绕回时没挪时间基准"
    assert rl.open_ is True


def test_replay_link_needs_telem_rows(tmp_path):
    """没有一帧遥测的回放文件要当场报错，而不是连上一条永远不吐帧的链路。"""
    path = tmp_path / "empty.jsonl"
    path.write_text('{"n": "log", "src": {"text": "只有日志"}}\n', encoding="utf-8")
    with pytest.raises(LinkError):
        ReplayLink(str(path)).start()
