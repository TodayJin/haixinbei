"""HTTP + SSE 服务：把 Console 的方法翻成 tools/console/docs/API.md 里的端点。

只用标准库（http.server），不引入 web 框架：桥的依赖被契约限制在
「Python 3.11+ 标准库 + pyserial」，装不上任何东西的工控笔记本也能跑。
静态文件服务 `tools/console/web/dist/`（前端 npm run build 的产物）。
"""
from __future__ import annotations

import json
import mimetypes
import os
import posixpath
import queue
import re
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .link import SerialLink, SimLink, LinkRunner, available_ports, find_default_port

VERSION = "1.0"
WEB_ROOT_DEFAULT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web", "dist")
SSE_BATCH_MS_DEFAULT = 20        # 0 = 不合并（一条事件一条报文，和提速前一样）


def group_sse(evs: list[dict], batch_max: int = 64) -> list[tuple[str, object]]:
    """把一批事件压成要发的 `[(事件名, 载荷)]`：**连续的遥测合成一条 `telem_batch`**。

    契约 §4：`data` 是事件载荷本身。单帧仍发 `telem`（形状不变），≥2 帧连在一起就发
    `telem_batch` + 载荷数组 —— 每条 SSE 报文都要 `event: …\\ndata: …\\n\\n` 二十来个字节，
    也各唤醒一次浏览器去解析：100Hz 下合并到 20ms 一报，报文数和唤醒数直接减半
    （实测见 `bridge.bench`）。`batch_max` 只限制**一条报文最多装几帧**，防偶发积压时
    吐出巨大报文；超出的部分留到下一组，不丢帧。非遥测事件（log/ack/stats/…）原样穿过。
    """
    out: list[tuple[str, object]] = []
    i = 0
    while i < len(evs):
        ev = evs[i]
        if ev.get("n") != "telem":
            out.append((ev.get("n", "msg"), ev.get("src", ev)))
            i += 1
            continue
        items: list[object] = []
        while i < len(evs) and evs[i].get("n") == "telem" and len(items) < batch_max:
            items.append(evs[i].get("src", evs[i]))
            i += 1
        out.append(("telem", items[0]) if len(items) == 1 else ("telem_batch", items))
    return out


class Bridge:
    """一条链路 + 一个 Console + 一个 HTTP 服务（`--serial` 可以在运行时换）。"""

    def __init__(self, mode: str = "sim", port: str = "", baud: int = 230400,
                 web_root: str = "", log_dir: str = "",
                 preset_dir: str = "", console_dir: str = "",
                 sse_batch_ms: int = SSE_BATCH_MS_DEFAULT):
        from .state import Console
        self.web_root = web_root or WEB_ROOT_DEFAULT
        self.console_dir = console_dir or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        self.log_dir = log_dir or os.path.join(self.console_dir, "data", "logs")
        self.preset_dir = preset_dir or os.path.join(self.console_dir, "data", "presets")
        os.makedirs(self.log_dir, exist_ok=True)
        os.makedirs(self.preset_dir, exist_ok=True)
        self.mode = mode
        self.port = port
        self.baud = baud
        self.sse_batch_ms = max(0, int(sse_batch_ms))
        self.runner: LinkRunner | None = None
        self.console: Console | None = None

    # ------------------------------------------------------------------ 链路
    # 串口名的形态：Windows 是 COM1 / COM26（也允许 `\\.\COM10` 的写法），POSIX 是 /dev/ttyXXX。
    # 为什么要在动旧链路之前先拦：出过一次事故 —— `link.port` 被写成一个不像串口名的文件名，
    # 设置页看着还像 COM26，点「连串口」就把桥切到这个"串口"名上；而 `SerialLink` 打不开时
    # 只在 note 里记一句、**不抛异常**，于是 switch() 认为建成了，把好的旧链路也停掉
    # ⇒ 表现为"操控不了车、遥测不动"。这里直接拒掉，旧链路就保得住。
    _RE_PORT_NT = re.compile(r"^(?:\\\\\.\\)?COM\d+$", re.IGNORECASE)
    _RE_PORT_POSIX = re.compile(r"^/dev/[A-Za-z0-9._/-]+$")

    @classmethod
    def _serial_port_ok(cls, port: str) -> bool:
        return bool((cls._RE_PORT_NT if os.name == "nt" else cls._RE_PORT_POSIX).match(port))

    def _make_link(self, mode: str, port: str, baud: int):
        if mode == "sim":
            return SimLink()
        port = port or find_default_port()
        if not port:
            raise SystemExit("没找到串口：用 --list-ports 看有哪些，或 --port COMxx")
        if not self._serial_port_ok(port):
            raise SystemExit(f"这不是串口号：{port}（Windows 上是 COM1、COM26 这种）")
        return SerialLink(port, baud)

    def start(self):
        link = self._make_link(self.mode, self.port, self.baud)
        if self.console is None:
            from .state import Console
            holder = {}
            self.console = Console.__new__(Console)      # 先建壳，再补 runner（构造器要 runner）
            Console.__init__(self.console, None, self.log_dir, self.preset_dir, self.console_dir)
            holder["c"] = self.console
        self.runner = LinkRunner(link, self._on_frame)
        self.console.runner = self.runner
        self.runner.start()
        self.console.log("info", f"链路已打开：{link.mode}"
                                 + (f" {link.port} @{link.baud}" if link.port and link.mode != "sim" else ""))
        # 开链先握手（真车才有应答；仿真会回一个）
        try:
            r = self.console.hello_req(timeout=0.8)
            if not r.get("answered"):
                self.console.log("warn", "没等到握手应答（车上固件可能还是老版本）")
        except Exception as e:
            self.console.log("warn", f"握手失败：{e}")
        return self

    def _on_frame(self, f, now_ms):
        self.console.on_frame(f, now_ms)
        if f.type in (0x01, 0x03):
            self.console.log_event({"t": now_ms, "n": "telem"})

    def stop(self):
        if self.runner:
            self.runner.stop()

    def switch(self, mode: str, port: str = "", baud: int = 0) -> dict:
        """运行中换链路（前端「连接」按钮）。

        先把新链路建起来（这一步会开串口），**能建才停旧的**：
        串口打不开时原来那条链路照旧在跑，前端正在看的波形不会被弄断
        （契约 §1 的失败语义）。
        """
        old_mode = self.mode
        try:
            link = self._make_link(mode, port or self.port, baud or self.baud)
        except (SystemExit, Exception) as e:        # SystemExit 是校验文案，Exception 是串口打不开
            msg = str(e) or e.__class__.__name__
            self.console.log("error", f"打开链路失败：{msg}")
            return {"ok": False, "error": msg}
        with self.console.lock:
            if self.runner:
                self.runner.stop()
            self.mode = mode
            if port:
                self.port = port
            if baud:
                self.baud = baud
        self.runner = LinkRunner(link, self._on_frame)
        self.console.runner = self.runner
        try:
            self.runner.start()
        except Exception as e:                      # 兜底：线程起来之后才爆的错
            self.console.log("error", f"链路启动失败：{e}")
            self.mode = old_mode
            return {"ok": False, "error": f"链路启动失败：{e}"}
        self.console.log("info", f"已切换链路：{link.mode} {link.port} @{link.baud}")
        r = self.console.hello_req(timeout=0.8)
        return {"ok": True, "link": self.runner.status(), "hello": self.console.hello,
                "answered": r.get("answered", False)}


class Handler(BaseHTTPRequestHandler):
    server_version = "quandi-console/1.0"
    protocol_version = "HTTP/1.1"

    # ------------------------------------------------------------------ 工具
    @property
    def bridge(self) -> Bridge:
        return self.server.bridge        # type: ignore[attr-defined]

    @property
    def con(self):
        return self.bridge.console

    def log_message(self, fmt, *args):   # 不往 stderr 灌访问日志（有自己的日志页）
        pass

    def _send(self, code: int, body: bytes, ctype: str = "application/json; charset=utf-8",
              extra: dict | None = None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "content-type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except Exception:
            pass

    def _json(self, obj, code: int = 200):
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def _body(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except Exception:
            n = 0
        if n <= 0:
            return {}
        raw = self.rfile.read(n)
        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            try:
                return {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode("utf-8")).items()}
            except Exception:
                return {}

    def _query(self) -> dict:
        _, _, q = self.path.partition("?")
        return {k: v[0] for k, v in urllib.parse.parse_qs(q).items()}

    # ------------------------------------------------------------------ 路由
    def do_OPTIONS(self):
        self._send(204, b"")

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        try:
            self._route_get(path)
        except BrokenPipeError:
            pass
        except Exception as e:
            self._json({"ok": False, "error": f"{type(e).__name__}: {e}"}, 500)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        try:
            self._route_post(path)
        except BrokenPipeError:
            pass
        except Exception as e:
            self._json({"ok": False, "error": f"{type(e).__name__}: {e}"}, 500)

    def do_DELETE(self):
        path = urllib.parse.urlparse(self.path).path
        try:
            if path == "/api/presets":
                name = str(self._query().get("name", "")).strip()
                if not name:
                    self._json({"ok": False, "error": "要删哪个预设？给 ?name="}, 400)
                    return
                self._json(self.con.preset_delete(name))
            else:
                self._json({"ok": False, "error": f"没有这个端点：{path}"}, 404)
        except BrokenPipeError:
            pass
        except Exception as e:
            self._json({"ok": False, "error": f"{type(e).__name__}: {e}"}, 500)

    # ------------------------------------------------------------------ GET
    def _route_get(self, path: str):
        con = self.con
        if path == "/api/health":
            st = con.runner.status()
            self._json({"ok": True, "version": VERSION,
                        "uptime_ms": int((time.time() - con.t0) * 1000),
                        "sim": st.get("mode") == "sim",
                        "mode": st.get("mode"), "link_open": st.get("open"),
                        "frames_seen": st.get("frames_seen", 0),
                        "telem_count": con.telem_count,
                        "note": st.get("note", ""),
                        "web_root": self.bridge.web_root,
                        "web_built": os.path.isfile(os.path.join(self.bridge.web_root, "index.html"))})
        elif path == "/api/link":
            self._json(con.link_payload())
        elif path == "/api/ports":
            ports = [{"port": p["device"], "desc": p.get("description", ""),
                      "likely": bool(p.get("likely")), "hwid": p.get("hwid", "")}
                     for p in available_ports()]
            self._json({"ok": True, "ports": ports,
                        "default": find_default_port(), "current": self.bridge.port})
        elif path == "/api/params":
            self._json({**con.params_payload(), "meta": con.params_meta()})
        elif path == "/api/params/meta":
            self._json({"ok": True, **con.params_meta()})
        elif path == "/api/actions":
            if not con.actions or self._query().get("refresh", "0") not in ("0", "", "false"):
                con.action_refresh()
            self._json(con.actions_payload())
        elif path == "/api/servo":
            self._json({"ok": True, **con.servo,
                        "dev1_us": con.servo.get("dev1_us", 0),
                        "dev2_us": con.servo.get("dev2_us", 0)})
        elif path == "/api/drive":
            self._json({"ok": True, "drive": con.drive_report})
        elif path == "/api/cal":
            force = self._query().get("refresh", "0") not in ("0", "", "false")
            if force or not con.cal:
                r = con.cal_refresh()
                if not r.get("ok") and not con.cal:
                    self._json({"ok": False, "error": r.get("error", "没收到标定数据")}, 200)
                    return
            self._json({"ok": True, **con.cal})
        elif path == "/api/presets":
            self._json({"ok": True, "items": con.preset_list()})
        elif path == "/api/log":
            q = self._query()
            n = max(1, min(5000, int(q.get("limit", 300))))
            self._json({"ok": True, "items": list(con.logs)[-n:]})
        elif path == "/api/state":
            self._json({"ok": True, **con.snapshot()})
        elif path == "/api/stream":
            self._stream()
        elif path == "/api/tasks":
            self._json(con.task_list())
        elif path == "/api/tasks/get":
            try:
                tid = int(self._query().get("id", "-1"))
            except ValueError:
                tid = -1
            if tid < 0:
                self._json({"ok": False, "error": "要给 id"}, 400)
                return
            self._json(con.task_get(tid))
        elif path == "/api/tasks/status":
            self._json(con.task_status_payload())
        elif path.startswith("/api/"):
            self._json({"ok": False, "error": f"没有这个端点：{path}"}, 404)
        else:
            self._static(path)

    # ------------------------------------------------------------------ POST
    def _route_post(self, path: str):
        con = self.con
        body = self._body()
        if path == "/api/link/connect":
            mode = body.get("mode") or ("serial" if body.get("port") or self.bridge.port else "serial")
            r = self.bridge.switch(mode, body.get("port", ""), int(body.get("baud", 0) or 0))
            self._json({**r, "link": con.link_payload()})
        elif path == "/api/link/disconnect":
            self.bridge.stop()
            con.log("info", "链路已断开")
            self._json({"ok": True, "link": con.link_payload()})
        elif path == "/api/link/hello":
            self._json(con.hello_req(timeout=float(body.get("timeout", 0.8))))
        elif path == "/api/params/set":
            if "id" not in body and "key" not in body:
                self._json({"ok": False, "error": "要给 id 或 key"}, 400)
                return
            self._json(con.param_set(body.get("id", body.get("key")), body.get("value")))
        elif path == "/api/params/push":
            self._json(con.param_push(ids=body.get("ids"), keys=body.get("keys")))
        elif path == "/api/params/reload":
            self._json(con.param_reload())
        elif path == "/api/params/defaults":
            self._json(con.param_defaults(ids=body.get("ids"), keys=body.get("keys")))
        elif path == "/api/params/save":
            self._json(con.param_save())
        elif path == "/api/actions/set":
            self._json(con.action_set(int(body["n"]), int(body["a1"]), int(body["a2"])))
        elif path == "/api/actions/save":
            self._json(con.action_save())
        elif path == "/api/actions/play":
            self._json(con.action_play(int(body.get("n", 0))))
        elif path == "/api/actions/refresh":
            self._json(con.action_refresh())
        elif path == "/api/servo":
            ang = body.get("angle", body.get("deg"))
            if ang is None:
                self._json({"ok": False, "error": "要给 angle"}, 400)
                return
            self._json(con.servo_set(int(body["ch"]), int(ang)))
        elif path == "/api/servo/set":
            self._json(con.servo_set(int(body["ch"]), int(body.get("deg", body.get("angle")))))
        elif path == "/api/servo/center":
            self._json(con.servo_center())
        elif path == "/api/servo/deviation":
            self._json(con.servo_deviation(int(body["ch"]), int(body["us"])))
        elif path == "/api/drive":
            self._json(con.drive(body.get("mode", "off"), body.get("vl"), body.get("vr"),
                                 body.get("base"), body.get("turn")))
        elif path == "/api/drive/stop":
            self._json(con.drive(0))
        elif path == "/api/cmd":
            self._json(con.cmd(body.get("cmd", body.get("name", "")), int(body.get("arg", 0))))
        elif path == "/api/presets":
            name = str(body.get("name", "")).strip()
            if not name:
                self._json({"ok": False, "error": "预设要有名字"}, 400)
                return
            targets = body.get("targets")
            if targets:
                targets = [str(t) for t in targets]
            kind = body.get("kind") or ("mixed" if (not targets or len(targets) > 1)
                                        else str(targets[0]))
            values = body.get("values")
            if values is None:
                values = self._collect_preset(kind, targets)
            # keys = 这份预设到底装了哪几类（前端列表要显示）——抓当前时按实际抓到的算
            keys = body.get("keys") or targets or sorted((values or {}).keys())
            self._json({"ok": True, "preset": con.preset_save(name, kind, keys,
                                                              body.get("note", ""), values)})
        elif path == "/api/presets/apply":
            name = str(body.get("name", "")).strip()
            self._json(con.preset_apply(name, body.get("targets")))
        elif path == "/api/log/clear":
            with con.lock:
                con.logs.clear()
            self._json({"ok": True})
        # ---- 车上任务程序（docs/任务编程.md §5）。语义性失败（空槽位、超时）一律
        #      200 + {ok:false,error}；只有"固件没这个能力"才 501 —— 前端把 404/501
        #      当成"桥/固件还没支持"，会把面板切成提示态。
        elif path == "/api/tasks/put":
            try:
                tid = int(body["id"])
                off = int(body.get("offset", 0))
                data = bytes.fromhex(str(body.get("hex", "")))
            except (KeyError, TypeError, ValueError):
                self._json({"ok": False, "error": "要给 id / offset / 偶数长度的 hex"}, 400)
                return
            self._json(con.task_put(tid, off, data))
        elif path == "/api/tasks/commit":
            try:
                tid = int(body["id"])
                length = int(body["len"])
                crc = int(body["crc"])
                flags = int(body.get("flags", 0) or 0)
            except (KeyError, TypeError, ValueError):
                self._json({"ok": False, "error": "要给 id、len、crc"}, 400)
                return
            self._json(con.task_commit(tid, length, crc, str(body.get("name", "")), flags))
        elif path == "/api/tasks/delete":
            try:
                tid = int(body["id"])
            except (KeyError, TypeError, ValueError):
                self._json({"ok": False, "error": "要给 id"}, 400)
                return
            self._json(con.task_delete(tid))
        elif path == "/api/tasks/run":
            try:
                tid = int(body["id"])
            except (KeyError, TypeError, ValueError):
                self._json({"ok": False, "error": "要给 id"}, 400)
                return
            self._json(con.task_run(tid))
        elif path == "/api/tasks/stop":
            self._json(con.task_stop())
        else:
            self._json({"ok": False, "error": f"没有这个端点：{path}"}, 404)

    def _collect_preset(self, kind: str, targets=None) -> dict:
        """把当前状态打包成预设（前端点「保存预设」时用）。"""
        con = self.con
        want = set(targets) if targets else set()
        vals: dict = {}
        if want:
            take = lambda t: t in want                                   # noqa: E731
        else:
            take = lambda t: kind in (t, "mixed")                        # noqa: E731
        if take("params"):
            vals["params"] = {it["key"]: it["value"] for it in con.params}
        if take("actions"):
            vals["actions"] = {str(a["n"]): [a["a1"], a["a2"]] for a in con.actions}
        if take("servo"):
            vals["servo"] = {"1": con.servo.get("s1_deg", 0), "2": con.servo.get("s2_deg", 0)}
        return vals

    # ------------------------------------------------------------------ SSE
    def _stream(self):
        """一路 SSE。`bridge.sse_batch_ms > 0` 时按固定网格攒批（见 `group_sse`）。

        攒批是个**有意的延迟交换**：报文最多晚 `sse_batch_ms` 毫秒发出去，换报文数与
        浏览器唤醒数减半。默认 20ms（100Hz 遥测 ≈ 每条报文 2 帧、显示晚半帧），
        `--sse-batch-ms 0` 就退回"一条事件一条报文"的老行为。
        """
        sub = self.con.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        batch = max(0, int(getattr(self.bridge, "sse_batch_ms", 0) or 0)) / 1000.0
        try:
            self._sse(sub, "state", self.con.snapshot())
            last_ping = time.time()
            pending: list[dict] = []
            next_flush = time.monotonic() + batch
            while True:
                pending += sub.pop_all(timeout=0.2 if batch else 1.0)
                now = time.monotonic()
                if pending and (not batch or now >= next_flush or len(pending) >= 128):
                    if batch:
                        for name, payload in group_sse(pending):
                            self._sse(sub, name, payload)
                    else:                       # 0 = 关：一事件一报文（老行为）
                        for ev in pending:
                            self._sse(sub, ev.get("n", "msg"), ev.get("src", ev))
                    pending = []
                    next_flush = now + batch
                    last_ping = time.time()
                elif time.time() - last_ping >= 15.0:
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
                    last_ping = time.time()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.con.unsubscribe(sub)

    def _sse(self, sub, name: str, obj):
        data = json.dumps(obj, ensure_ascii=False)
        self.wfile.write(f"event: {name}\ndata: {data}\n\n".encode("utf-8"))
        self.wfile.flush()

    # ------------------------------------------------------------------ 静态
    def _static(self, path: str):
        root = self.bridge.web_root
        if not os.path.isdir(root):
            self._json({"ok": False,
                        "error": f"前端还没构建：{root} 不存在。"
                                 f"先 `cd tools/console/web && npm install && npm run build`，"
                                 f"或者用 `mock/server.mjs` 只调前端。"}, 404)
            return
        rel = posixpath.normpath(urllib.parse.unquote(path)).lstrip("/")
        if rel in ("", "."):
            rel = "index.html"
        full = os.path.join(root, rel)
        if not os.path.abspath(full).startswith(os.path.abspath(root)):
            self._json({"ok": False, "error": "路径越界"}, 403)
            return
        if not os.path.isfile(full):                    # SPA 回落
            full = os.path.join(root, "index.html")
        if not os.path.isfile(full):
            self._json({"ok": False, "error": "index.html 不存在（前端没构建）"}, 404)
            return
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        with open(full, "rb") as fh:
            body = fh.read()
        self._send(200, body, ctype)


def serve(bridge: Bridge, host: str = "127.0.0.1", port: int = 8770):
    httpd = ThreadingHTTPServer((host, port), Handler)
    httpd.daemon_threads = True
    httpd.bridge = bridge           # type: ignore[attr-defined]
    return httpd
