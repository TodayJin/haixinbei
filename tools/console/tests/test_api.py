"""HTTP 契约测试：把 docs/API.md 里承诺的端点全部打一遍（跑在 --sim 上，不碰真车）。

`live` fixture 起的是真 `Bridge(mode="sim")` + 真 `ThreadingHTTPServer`，所以这里
覆盖的是"前端 → HTTP → 桥 → 链路 → 帧"整流。真车相关的部分（串口）由 `--serial`
手工验证，自动化测试不允许碰电机/舵机。
"""
from __future__ import annotations

import json
import time
import urllib.error
import urllib.parse
import urllib.request

import pytest

from bridge import params as P
from bridge import protocol as pr


def test_health_and_link(live):
    base, bridge, con = live
    code, j, _ = GET(base + "/api/health")
    assert code == 200 and j["ok"] is True
    assert j["sim"] is True and j["link_open"] is True
    assert isinstance(j["uptime_ms"], int) and j["version"]
    assert "web_built" in j and "telem_count" in j

    # 契约 §3.1：/api/link 返回的就是 LinkInfo 本身（没有 link 外壳）
    code, j, _ = GET(base + "/api/link")
    assert code == 200 and j["ok"] is True
    assert j["mode"] == "sim" and j["connected"] is True
    assert j["baud"] == 115200 and isinstance(j["telem_hz"], (int, float))
    assert set(j["stats"]) == {"rx", "tx", "crc", "resync", "drops"}
    assert j["stats"]["rx"] >= 1, "仿真车已经在发遥测了"
    assert j["last_rx_ms"] is None or j["last_rx_ms"] >= 0
    assert j["fw"] is None or set(j["fw"]) == {"name", "version", "proto",
                                               "param_crc", "telem_hz", "caps"}


def test_hello(live):
    base, _, _ = live
    code, j, _ = POST(base + "/api/link/hello", {"timeout": 1.0})
    assert code == 200 and j["ok"] is True
    code, j, _ = GET(base + "/api/link")
    assert j["fw"]["proto"] == 2 and j["fw"]["name"] == "quandi-car"
    assert j["fw"]["caps"] == 0x7F


def test_ports_endpoint(live):
    base, _, _ = live
    code, j, _ = GET(base + "/api/ports")
    assert code == 200 and isinstance(j["ports"], list)
    assert "default" in j and "current" in j
    for p in j["ports"]:
        assert {"port", "desc"} <= set(p), "契约里端口项是 {port, desc}"


def test_params_contract_fields(live):
    base, _, _ = live
    code, j, _ = GET(base + "/api/params")
    assert code == 200 and len(j["items"]) == len(P.PARAMS)
    assert j["source"] in ("fw", "local") and isinstance(j["param_crc"], int)
    fields = {"id", "key", "label", "group", "unit", "value", "default", "min", "max",
              "decimals", "step", "dirty", "note"}
    for it in j["items"]:
        assert fields <= set(it), fields - set(it)
        assert it["group"] in P.GROUPS
    assert j["meta"]["groups"] == P.GROUPS
    code, j, _ = GET(base + "/api/params/meta")
    assert code == 200 and j["ok"] is True and len(j["items"]) == len(P.PARAMS)


def test_param_set_push_reload_defaults(live):
    base, _, con = live
    # 1) set 只改桥这边的缓存，不自动下发（契约 §2：点了「下发」才到车）
    code, j, _ = POST(base + "/api/params/set", {"key": "pivot_kp", "value": 1200})
    assert code == 200 and j["ok"] is True and j["pushed"] is False
    item = next(i for i in GET(base + "/api/params")[1]["items"] if i["key"] == "pivot_kp")
    assert item["value"] == pytest.approx(1200.0)
    assert item["dirty"] is True, "改了还没下发就是脏"

    code, j, _ = POST(base + "/api/params/set", {"id": 0x0207, "value": 1200})
    assert j["ok"] is True, "id 形式和 key 形式都要收"
    code, j, _ = POST(base + "/api/params/set", {"key": "pivot_kp", "value": 99999})
    assert j["ok"] is False, "越界必须被桥挡住"
    code, j, _ = POST(base + "/api/params/set", {"key": "没有这个键", "value": 1})
    assert j["ok"] is False

    # 2) push 才真的到车；车一认下来就不脏了
    code, j, _ = POST(base + "/api/params/push", {})
    assert code == 200 and j["ok"] is True and j["sent"] == j["of"] >= 1
    item = next(i for i in GET(base + "/api/params")[1]["items"] if i["key"] == "pivot_kp")
    assert item["value"] == pytest.approx(1200.0) and item["dirty"] is False

    # 3) defaults 只改本机表并标脏（等 push）；reload 从车读回 = 刚才下发的值
    code, j, _ = POST(base + "/api/params/defaults", {"keys": ["pivot_kp"]})
    assert j["ok"] is True and j["pushed"] is False and j["keys"] == ["pivot_kp"]
    item = next(i for i in GET(base + "/api/params")[1]["items"] if i["key"] == "pivot_kp")
    assert item["value"] == pytest.approx(item["default"]) and item["dirty"] is True

    code, j, _ = POST(base + "/api/params/reload", {})
    assert j["ok"] is True and j["read"] >= 1
    item = next(i for i in GET(base + "/api/params")[1]["items"] if i["key"] == "pivot_kp")
    assert item["value"] == pytest.approx(1200.0), "reload 之后要回到车上的值"
    assert item["dirty"] is False

    code, j, _ = POST(base + "/api/params/save", {})
    assert j["ok"] is True


def test_preset_apply_reaches_the_car(live):
    """预设里的参数必须真的下发到车（不是只改桥这边的表）。"""
    base, _, _ = live
    POST(base + "/api/params/set", {"key": "pivot_kd", "value": 111})
    code, j, _ = POST(base + "/api/params/push", {"keys": ["pivot_kd"]})
    assert j["ok"] is True and j["sent"] == 1
    code, j, _ = POST(base + "/api/presets", {"name": "__t_preset__", "kind": "params"})
    assert j["ok"] is True
    POST(base + "/api/params/set", {"key": "pivot_kd", "value": 222})
    POST(base + "/api/params/push", {"keys": ["pivot_kd"]})
    code, j, _ = POST(base + "/api/presets/apply",
                      {"name": "__t_preset__", "targets": ["params"]})
    assert code == 200 and j["ok"] is True and j["sent"] >= 1
    code, j, _ = POST(base + "/api/params/reload", {})
    item = next(i for i in GET(base + "/api/params")[1]["items"] if i["key"] == "pivot_kd")
    assert item["value"] == pytest.approx(111.0), "应用预设后车上应该是预设里的值"
    assert item["dirty"] is False
    assert DELETE(base + "/api/presets?name=__t_preset__")[1]["ok"] is True


def test_actions_endpoints(live):
    base, _, _ = live
    code, j, _ = GET(base + "/api/actions")
    assert code == 200 and len(j["items"]) == pr.ACTION_COUNT
    fields = {"n", "name", "a1", "a2", "preset_a1", "preset_a2", "dirty", "compound", "steps"}
    for it in j["items"]:
        assert fields <= set(it), fields - set(it)
    assert j["items"][0]["n"] == 1

    code, j, _ = POST(base + "/api/actions/set", {"n": 3, "a1": 100, "a2": 20})
    assert code == 200 and j["ok"] is True
    acts = GET(base + "/api/actions")[1]["items"]
    assert (acts[2]["a1"], acts[2]["a2"]) == (100, 20) and acts[2]["dirty"] is True
    code, j, _ = POST(base + "/api/actions/set", {"n": 99, "a1": 10, "a2": 10})
    assert j["ok"] is False
    assert POST(base + "/api/actions/play", {"n": 3})[1]["ok"] is True
    assert POST(base + "/api/actions/play", {"n": 0})[1]["ok"] is True
    assert POST(base + "/api/actions/save", {})[1]["ok"] is True
    assert POST(base + "/api/actions/refresh", {})[1]["ok"] is True


def test_servo_endpoints(live):
    base, _, _ = live
    code, j, _ = GET(base + "/api/servo")
    assert code == 200 and {"s1_us", "s2_us", "s1_deg", "s2_deg"} <= set(j)
    assert POST(base + "/api/servo/set", {"ch": 1, "deg": 45})[1]["ok"] is True
    assert POST(base + "/api/servo/set", {"ch": 3, "deg": 45})[1]["ok"] is False
    assert POST(base + "/api/servo/set", {"ch": 1, "deg": 200})[1]["ok"] is False
    assert POST(base + "/api/servo/deviation", {"ch": 1, "us": 30})[1]["ok"] is True
    assert POST(base + "/api/servo/deviation", {"ch": 1, "us": 9999})[1]["ok"] is False
    assert POST(base + "/api/servo/center", {})[1]["ok"] is True


def test_drive_endpoints(live):
    base, _, con = live
    code, j, _ = POST(base + "/api/drive", {"mode": "wheels", "vl": 300, "vr": -300})
    assert code == 200 and j["ok"] is True, j
    code, j, _ = GET(base + "/api/drive")
    assert j["drive"]["mode"] == 1 and j["drive"]["enabled"] is True
    assert j["drive"].get("a") == 300 and j["drive"].get("b") == -300

    code, j, _ = POST(base + "/api/drive", {"mode": "base_turn", "base": 4000, "turn": 30})
    assert j["ok"] is True
    code, j, _ = POST(base + "/api/drive", {"mode": "trace", "base": 2500})
    assert j["ok"] is True
    assert POST(base + "/api/drive", {"mode": "乱写的"})[1]["ok"] is False
    assert POST(base + "/api/drive/stop", {})[1]["ok"] is True
    assert GET(base + "/api/drive")[1]["drive"]["enabled"] is False


def test_cmd_endpoint(live):
    base, _, con = live
    assert POST(base + "/api/cmd", {"cmd": "stop"})[1]["ok"] is True
    assert POST(base + "/api/cmd", {"cmd": "clear_fault"})[1]["ok"] is True
    assert POST(base + "/api/cmd", {"cmd": "set_telem_hz", "arg": 20})[1]["ok"] is True
    assert POST(base + "/api/cmd", {"cmd": "开始跑"})[1]["ok"] is False
    # 仿真里没实现的命令要老实回"未就绪"，不能假装成功
    r = POST(base + "/api/cmd", {"cmd": "reset"})[1]
    assert r["ok"] is False


def test_cal_endpoint(live):
    base, _, _ = live
    code, j, _ = GET(base + "/api/cal?refresh=1")
    assert code == 200 and j["ok"] is True, j
    assert len(j["ch"]) == 8
    for ch in j["ch"]:
        assert {"i", "blk", "wht", "thr", "jitter", "pol", "ok", "reason",
                "level", "thr_level"} <= set(ch)
    assert j["active_set"] == 0 and 0 <= j["quality"] <= 8


def test_state_and_log_endpoints(live):
    base, _, _ = live
    code, j, _ = GET(base + "/api/state")
    assert code == 200 and j["ok"] is True
    assert "names" in j and "mode" in j["names"]
    code, j, _ = GET(base + "/api/log?limit=10")
    assert code == 200 and isinstance(j["items"], list)
    assert POST(base + "/api/log/clear", {})[1]["ok"] is True
    assert GET(base + "/api/log")[1]["items"] == []


def test_presets_roundtrip(live):
    base, _, _ = live
    code, j, _ = POST(base + "/api/presets", {"name": "测试预设", "targets": ["params"],
                                              "note": "pytest"})
    assert code == 200 and j["ok"] is True
    names = [p["name"] for p in GET(base + "/api/presets")[1]["items"]]
    assert "测试预设" in names
    # 不带 targets（前端「保存当前」）：keys 要写出这份预设装了哪几类，否则列表显示不全
    code, j, _ = POST(base + "/api/presets", {"name": "全部预设", "note": "pytest"})
    assert code == 200 and j["ok"] is True
    full = next(p for p in GET(base + "/api/presets")[1]["items"] if p["name"] == "全部预设")
    assert set(full["keys"]) == {"params", "actions", "servo"}, full["keys"]
    assert http_get(base + "/api/presets?name=" + urllib.parse.quote("全部预设"),
                    method="DELETE")[1]["ok"] is True
    assert POST(base + "/api/presets", {"name": ""})[1]["ok"] is False
    code, j, _ = POST(base + "/api/presets/apply", {"name": "测试预设"})
    assert code == 200, j
    code, j, _ = POST(base + "/api/presets/apply", {"name": "不存在"})
    assert j["ok"] is False
    # DELETE /api/presets?name=…（契约里有，前端「删除预设」用它）
    assert http_get(base + "/api/presets?name=" + urllib.parse.quote("测试预设"),
                    method="DELETE")[1]["ok"] is True
    assert "测试预设" not in [p["name"] for p in GET(base + "/api/presets")[1]["items"]]


def test_record_endpoint(live):
    base, _, _ = live
    code, j, _ = POST(base + "/api/record", {"on": True})
    assert code == 200 and j["ok"] is True
    assert j["record"]["recording"] is True and j["record"]["path"]
    time.sleep(0.3)
    code, j, _ = POST(base + "/api/record", {"on": False})
    assert j["record"]["recording"] is False
    path = j["record"]["path"]
    assert path and j["record"]["samples"] >= 1
    with open(path, "r", encoding="utf-8") as fh:
        lines = [json.loads(x) for x in fh if x.strip()]
    assert lines and any(x.get("n") == "telem" for x in lines)
    # GET /api/record 也要给同样三个字段
    code, j, _ = GET(base + "/api/record")
    assert code == 200 and set(j["record"]) >= {"ok", "recording", "path", "samples"}


def test_unknown_endpoint_and_cors(live):
    base, _, _ = live
    code, j, raw = GET(base + "/api/nope")
    assert code == 404 and j["ok"] is False
    assert "没有这个端点" in j["error"]


def test_static_index_or_hint(live):
    """dist 没构建时要给中文提示而不是 404 白屏。"""
    base, _, _ = live
    req = urllib.request.Request(base + "/", method="GET")
    with urllib.request.urlopen(req, timeout=5) as r:
        body = r.read()
        assert r.status == 200
    assert b"npm run build" in body or b"<!doctype html" in body.lower()


def test_sse_first_event_is_state(live):
    base, _, _ = live
    req = urllib.request.Request(base + "/api/stream", headers={"Accept": "text/event-stream"})
    with urllib.request.urlopen(req, timeout=5) as r:
        assert r.headers.get("Content-Type", "").startswith("text/event-stream")
        buf = b""
        t0 = time.time()
        while time.time() - t0 < 4 and b"\n\n" not in buf:
            buf += r.read1(4096) if hasattr(r, "read1") else r.read(1024)
        text = buf.decode("utf-8", "replace")
    assert "event: state" in text
    payload = text.split("data:", 1)[1].split("\n", 1)[0]
    j = json.loads(payload)
    # 契约 §4：state 事件 = {link, params, actions, presets, cal}，data 就是载荷本身
    for key in ("link", "params", "actions", "presets", "cal"):
        assert key in j, f"state 事件缺 {key}"
    assert j["link"]["mode"] == "sim"
    assert isinstance(j["params"]["items"], list) and j["params"]["items"]
    assert isinstance(j["actions"]["items"], list) and j["actions"]["items"]
    assert isinstance(j["presets"], list)


# --------------------------------------------------------------------- 小工具
def GET(url: str):
    return http_get(url)


def POST(url: str, body: dict):
    return http_get(url, method="POST", body=body)


def DELETE(url: str):
    return http_get(url, method="DELETE")


def http_get(url: str, method: str = "GET", body: dict | None = None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=6) as r:
            raw = r.read()
            return r.status, json.loads(raw.decode("utf-8")), raw
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw.decode("utf-8")), raw
        except Exception:
            return e.code, None, raw
