"""共用夹具：让 `python -m pytest tools/console/tests -q` 从仓库根跑得起来。"""
from __future__ import annotations

import os
import sys
import threading
import urllib.request

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE = os.path.dirname(HERE)                 # tools/console
BRIDGE = os.path.join(CONSOLE, "bridge")
REPO = os.path.dirname(os.path.dirname(CONSOLE))

sys.path.insert(0, CONSOLE)                     # 让 `bridge` 成为可导入包


def http_json(url: str, method: str = "GET", body: bytes | None = None, timeout: float = 8.0):
    """返回 (status, json 或 None, 原始文本)。"""
    req = urllib.request.Request(url, data=body, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            status = r.status
    except urllib.error.HTTPError as e:            # 4xx 也要能读 body
        raw = e.read().decode("utf-8", "replace")
        status = e.code
    import json
    try:
        return status, json.loads(raw), raw
    except Exception:
        return status, None, raw


def post_json(url: str, obj, timeout: float = 8.0):
    import json
    return http_json(url, "POST", json.dumps(obj).encode("utf-8"), timeout)


@pytest.fixture(scope="session")
def live(tmp_path_factory):
    """起一个 --sim 的桥（随机端口），产出 (base_url, bridge, console)。

    预设与日志目录指到临时目录：测试不许往仓库里写文件（否则 `actions/save`
    会把预设写进 tools/console/data/presets/，第二次跑就不干净了）。
    """
    from bridge.server import Bridge, serve
    tmp = tmp_path_factory.mktemp("console")
    b = Bridge(mode="sim", console_dir=CONSOLE,
               preset_dir=os.path.join(str(tmp), "presets"),
               log_dir=os.path.join(str(tmp), "logs"))
    b.start()
    httpd = serve(b, "127.0.0.1", 0)
    port = httpd.server_address[1]
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    try:
        yield f"http://127.0.0.1:{port}", b, b.console
    finally:
        httpd.shutdown()
        b.stop()


@pytest.fixture(scope="session")
def sim():
    """直接拿仿真车本体（不走 HTTP）。"""
    from bridge.sim import SimCar
    car = SimCar()
    car.start(1000)
    return car
