"""控制台状态机：链路上的收发、参数/动作/标定缓存、日志、录制、SSE 广播。

只有这一层懂业务；HTTP 层（server.py）只是把它的方法翻译成 JSON。
所有对车的写操作都走 `send_wait()`：发帧 → 等应答（ACK / 报表）→ 返回结果，
这样前端点一下就能立刻知道车到底接受了没有（契约里的 `ack`/`status`）。
"""
from __future__ import annotations

import json
import os
import threading
import time
from collections import deque

from . import params as P
from . import protocol as pr

LOG_MAX = 800
EVENT_MAX = 4000          # 每路订阅者最多积压多少事件（防前端卡住把内存吃光）

CMD_NAMES = {
    "stop": pr.CMD_STOP, "start_a": pr.CMD_START_A, "start_b": pr.CMD_START_B,
    "start_c": pr.CMD_START_C, "calib_gyro": pr.CMD_CALIB_GYRO,
    "save_params": pr.CMD_SAVE_PARAMS, "clear_fault": pr.CMD_CLEAR_FAULT,
    "set_telem_hz": pr.CMD_SET_TELEM_HZ, "reset": pr.CMD_RESET,
    "set_logging": pr.CMD_SET_LOGGING, "set_telem_mode": pr.CMD_SET_TELEM_MODE,
}
DRIVE_MODES = {"off": 0, "wheels": 1, "base_turn": 2, "trace": 3}
DRIVE_KEEPALIVE_MS = 200      # 车上驾驶看门狗是 500ms，留 2 倍余量
DRIVE_HOLD_MS = 3000          # 有 /api/stream 订阅者挂着就算"在场"，超时就不再喂
RENWU_NAMES = {0: "空闲", 1: "任务A", 2: "任务B", 3: "任务C"}
MODE_NAMES = {0: "空闲", 1: "陀螺", 2: "循迹", 3: "串级"}
NVRAM_NAMES = {0: "空闲", 1: "待保存", 2: "保存中", 3: "已保存", 4: "失败"}
ST_NAMES = {0x00: "OK", 0x02: "未知类型", 0x03: "参数错", 0x04: "超范围",
            0x05: "未就绪", 0x06: "命令错", 0x07: "忙"}
# ACK 事件里给前端看的"这是哪个请求的应答"（契约 §4 `ack.req`）
REQ_NAMES = {
    pr.T_PARAM_SET: "params.set", pr.T_PARAM_GET: "params.push", pr.T_PARAM_SAVE: "params.save",
    pr.T_ACTION_SET: "actions.set", pr.T_ACTION_SAVE: "actions.save",
    pr.T_SERVO_SET: "servo.set", pr.T_SERVO_PLAY: "servo.play",
    pr.T_SERVO_DEV: "servo.deviation",
    pr.T_DRIVE: "drive", pr.T_CMD: "cmd", pr.T_CAL_GET: "cal", pr.T_HELLO: "hello",
    pr.T_ACTIONS_GET: "actions.refresh",
}


class Subscriber:
    """一路 SSE 订阅者：一个事件队列 + 一个条件变量。"""

    def __init__(self):
        self.q: deque = deque(maxlen=EVENT_MAX)
        self.cv = threading.Condition()
        self.dropped = 0

    def push(self, ev: dict):
        with self.cv:
            if len(self.q) == self.q.maxlen:
                self.dropped += 1
            self.q.append(ev)
            self.cv.notify()

    def pop_all(self, timeout: float = 15.0) -> list[dict]:
        with self.cv:
            if not self.q:
                self.cv.wait(timeout)
            out = list(self.q)
            self.q.clear()
            return out


class Console:
    def __init__(self, runner, log_dir: str, preset_dir: str, console_dir: str):
        self.runner = runner
        self.log_dir = log_dir
        self.preset_dir = preset_dir
        self.console_dir = console_dir
        self.lock = threading.RLock()
        self.cv = threading.Condition(self.lock)
        self.t0 = time.time()

        self.hello: dict = {}
        self.telem: dict = {}
        self.telem_host_ms = 0
        self.telem_count = 0
        self.link_note = ""
        self.drive_report: dict = {}
        self.servo = {"s1_us": 1500, "s2_us": 1500, "s1_deg": 0, "s2_deg": 0,
                      "dev1_us": 0, "dev2_us": 0}
        self.params: list[dict] = []
        self.car_values: dict[int, float] = {}
        self.actions: list[dict] = []
        self.action_vals = [list(v) for v in P.ACTION_FACTORY]
        self.action_preset = self._load_action_preset()
        self._rebuild_actions()
        self.cal: dict = {}
        self.logs: deque = deque(maxlen=LOG_MAX)
        self.replies: dict[int, list[pr.Frame]] = {}
        self.subs: list[Subscriber] = []
        self.hz = 50
        self.telem_mode = 1
        self.rec_path = ""
        self.recording = False
        self.rec_samples = 0
        self.rec_started: int | None = None
        self._rec_fh = None
        self._seq = 0
        self.rtt_ms: float | None = None
        self._sent_type: dict[int, int] = {}
        self._sent_name: dict[int, str] = {}
        self.stats = {"rx_frames": 0, "tx_frames": 0, "rx_bytes": 0, "tx_bytes": 0,
                      "crc_errors": 0, "resyncs": 0, "t": int(time.time() * 1000)}
        self._applied_preset = ""
        self._last_stats_ms = 0
        self.drive_hold_until = 0
        self.drive_ka_ms = 0
        self.drive_cmd: dict = {}
        self._rate = {"telem": 0, "tx": 0}
        self._rate_t = time.time()
        self._rate_n = {"telem": 0, "tx": 0}
        self.reload_params()

    # ------------------------------------------------------------------ 参数表
    def reload_params(self):
        """把参数表结构（key/label/group/单位/默认/范围）建起来；值来自车（没连车就用默认）。"""
        with self.lock:
            items = []
            for sp in P.PARAMS:
                v = self.car_values.get(sp.id, sp.default)
                items.append({
                    "id": sp.id, "key": sp.key, "label": sp.label, "group": sp.group,
                    "unit": sp.unit, "value": v, "default": sp.default,
                    "min": sp.min, "max": sp.max, "decimals": sp.decimals,
                    "step": sp.step, "dirty": False, "note": sp.note,
                })
            self.params = items
        return self.params

    def param_item(self, id_or_key) -> dict | None:
        sp = P.by_id_or_key(id_or_key)
        if sp is None:
            return None
        with self.lock:
            for it in self.params:
                if it["id"] == sp.id:
                    return it
        return None

    def _set_car_value(self, pid: int, v: float):
        self.car_values[pid] = v
        for it in self.params:
            if it["id"] == pid:
                it["value"] = v
                it["dirty"] = False

    # ------------------------------------------------------------------ 广播
    def _now_ms(self) -> int:
        """毫秒时钟 —— 必须和 LinkRunner 传进来的 now_ms 同一把尺子（那里用的是墙上时钟）。"""
        return int(time.time() * 1000)

    def subscribe(self) -> Subscriber:
        s = Subscriber()
        with self.lock:
            self.subs.append(s)
        return s

    def unsubscribe(self, s: Subscriber):
        with self.lock:
            if s in self.subs:
                self.subs.remove(s)

    def emit(self, ev: dict):
        ev.setdefault("t", int(time.time() * 1000))
        with self.lock:
            subs = list(self.subs)
        for s in subs:
            s.push(ev)

    def log(self, level: str, msg: str):
        item = {"ts_ms": int(time.time() * 1000), "level": level, "text": msg}
        with self.lock:
            self.logs.append(item)
        self.emit({"n": "log", "src": item})
        self.record({"n": "log", "src": item})

    def record(self, obj: dict):
        if not self.recording or self._rec_fh is None:
            return
        try:
            with self.lock:
                self._rec_fh.write(json.dumps(obj, ensure_ascii=False) + "\n")
                self._rec_fh.flush()
                if obj.get("n") == "telem":
                    self.rec_samples += 1
        except Exception:
            pass

    # ------------------------------------------------------------------ 收发
    def next_seq(self) -> int:
        with self.lock:
            self._seq = (self._seq + 1) & 0xFF
            if self._seq == 0:
                self._seq = 1
            return self._seq

    def send(self, type_: int, payload: bytes = b"", seq: int | None = None,
             req: str | None = None) -> int:
        if seq is None:
            seq = self.next_seq()
        with self.lock:
            if len(self._sent_type) > 512:
                self._sent_type.clear()
                self._sent_name.clear()
            self._sent_type[seq] = type_
            if req:
                self._sent_name[seq] = req
        data = pr.build_frame(type_, seq, payload)
        self.runner.send(data)
        return seq

    def send_wait(self, type_: int, payload: bytes = b"", want=(pr.T_ACK,),
                  timeout: float = 0.6, req: str | None = None) -> dict:
        """发一帧并等应答。返回 {'ok','status','status_name','frames',...}。"""
        seq = self.next_seq()
        with self.cv:
            self.replies.pop(seq, None)
        t_sent = time.time()
        self.send(type_, payload, seq, req=req)
        got: list[pr.Frame] = []
        deadline = t_sent + timeout
        while time.time() < deadline:
            with self.cv:
                if seq in self.replies:
                    got = self.replies.pop(seq)
                    break
                self.cv.wait(max(0.01, deadline - time.time()))
        st = None
        for f in got:
            if f.type == pr.T_ACK and len(f.payload) >= 2:
                st = f.payload[1]
        if got:
            with self.lock:
                self.rtt_ms = round((time.time() - t_sent) * 1000.0, 1)
        return {
            "seq": seq, "sent": True,
            "status": st if st is not None else None,
            "status_name": ST_NAMES.get(st, "无应答" if st is None else f"未知({st:#04x})"),
            "ok": (st == pr.ST_OK) if st is not None else False,
            "answered": bool(got),
            "frames": [{"type": f.type, "payload_hex": f.payload.hex()} for f in got],
        }

    # ------------------------------------------------------------------ 收帧
    def on_frame(self, f: pr.Frame, now_ms: int):
        link_st = self.runner.status()
        with self.lock:
            self.stats.update({k: link_st.get(k, 0) for k in
                               ("rx_frames", "tx_frames", "rx_bytes", "tx_bytes",
                                "crc_errors", "resyncs")})
            self.stats["t"] = int(time.time() * 1000)
        try:
            self._dispatch(f, now_ms)
        except Exception as e:                    # 单帧坏数据不许打死链路
            self.log("error", f"处理帧 0x{f.type:02x} 出错：{e}")
        if f.seq:
            with self.cv:
                self.replies.setdefault(f.seq, []).append(f)
                self.cv.notify_all()
        self._maybe_stats()
        self.drive_keepalive(now_ms)

    # -------------------------------------------------------- 驾驶看门狗保持
    def drive_keepalive(self, now_ms: int):
        """车上的驾驶看门狗是 500ms：上位机只要还"在场"，就每 200ms 喂一次。

        "在场" = 有人挂着 /api/stream（浏览器开着）；浏览器关掉后 3s 内不再喂，
        车自己会停 —— 这是刻意的失效方向（宁可自己停，也不要失控跑）。
        """
        with self.lock:
            if not self.drive_hold_until:
                return
            if self.subs:
                self.drive_hold_until = now_ms + DRIVE_HOLD_MS
            if now_ms > self.drive_hold_until:
                self.drive_hold_until = 0
                self.drive_ka_ms = 0
                expired = True
            else:
                expired = False
                due = now_ms - self.drive_ka_ms >= DRIVE_KEEPALIVE_MS
                if due:
                    self.drive_ka_ms = now_ms
        if expired:
            self.log("warn", "驾驶保持超时（上位机可能已关闭）——不再喂看门狗，车会自己停")
            self.emit({"n": "drive", "src": {"mode": 0, "enabled": False, "hold_expired": True}})
            return
        if due:
            self.send(pr.T_DRIVE_REPORT)          # 无害的查询帧，作用是喂看门狗

    def _dispatch(self, f: pr.Frame, now_ms: int):
        if f.type in (pr.T_TELEM, pr.T_TELEM2):
            t = pr.decode_telemetry(f.payload)
            t["t_host"] = int(now_ms)
            with self.lock:
                self.telem = t
                self.telem_host_ms = int(now_ms)
                self.telem_count += 1
                self._rate_n["telem"] += 1
                self.servo["s1_us"] = t["servo1_us"]
                self.servo["s2_us"] = t["servo2_us"]
                self.servo["s1_deg"] = t["servo1_deg"]
                self.servo["s2_deg"] = t["servo2_deg"]
                self.drive_report = dict(t.get("drive", {}), pwm_l=t.get("pwm_l"),
                                         pwm_r=t.get("pwm_r"), vl=t.get("vl"),
                                         vr=t.get("vr"))
                if self.drive_cmd:            # 车的遥测只带 mode/enabled，指令值由桥记住
                    self.drive_report = dict(self.drive_cmd, **self.drive_report)
            ev = {"n": "telem", "src": t, "t_host": int(now_ms)}
            self.emit(ev)
            self.record(ev)
        elif f.type == pr.T_HELLO:
            h = pr.decode_hello(f.payload)
            with self.lock:
                self.hello = h
                if h.get("telem_hz"):
                    self.hz = h["telem_hz"]
            self.log("info", f"握手：固件 v{h['version']} 协议 v{h['proto']} "
                             f"能力位 0x{h['caps']:02X} 名字 {h['name']}")
            self.emit({"n": "hello", "src": h})
        elif f.type == pr.T_PARAM_REPORT:
            pid, v = pr.decode_param_report(f.payload)
            with self.lock:
                self._set_car_value(pid, v)
                item = next((i for i in self.params if i["id"] == pid), None)
            if item:
                self.emit({"n": "params", "src": self.params_payload()})
        elif f.type == pr.T_ACTIONS:
            acts = pr.decode_actions(f.payload)
            with self.lock:
                if len(acts) == P.ACTION_COUNT:
                    self.action_vals = [[int(a), int(b)] for a, b in acts]
                    self._rebuild_actions()
            self.emit({"n": "actions", "src": self.actions_payload()})
        elif f.type == pr.T_CAL_REPORT:
            with self.lock:
                self.cal = pr.decode_cal(f.payload)
            self.emit({"n": "cal", "src": self.cal})
        elif f.type == pr.T_DRIVE_REPORT:
            d = pr.decode_drive_report(f.payload)
            with self.lock:
                self.drive_report = dict(self.drive_cmd, **d) if self.drive_cmd else d
            self.emit({"n": "drive", "src": self.drive_report})
        elif f.type == pr.T_ACK:
            if len(f.payload) >= 2:
                st = f.payload[1]
                with self.lock:
                    nm = self._sent_name.pop(f.seq, None)
                    ty = self._sent_type.pop(f.seq, 0)
                req = nm or REQ_NAMES.get(ty, "cmd")
                self.emit({"n": "ack", "src": {
                    "seq": f.seq, "req": req, "status": st, "ok": (st == pr.ST_OK),
                    "text": ST_NAMES.get(st, f"{st:#04x}"),
                    "status_name": ST_NAMES.get(st, f"{st:#04x}")}})
        elif f.type == pr.T_TEXT:
            self.log("info", "[车] " + pr.decode_text(f.payload))
        else:
            self.emit({"n": "frame", "src": {"type": f.type, "seq": f.seq,
                                             "len": len(f.payload)}})

    def _maybe_stats(self):
        now = time.time()
        with self.lock:
            if now - self._rate_t >= 1.0:
                dt = max(1e-3, now - self._rate_t)
                self._rate = {k: int(v / dt) for k, v in self._rate_n.items()}
                self._rate_n = {k: 0 for k in self._rate_n}
                self._rate_t = now
                stats = dict(self.stats, telem_hz=self._rate["telem"], tx_hz=self._rate["tx"],
                             frames_seen=self.runner.frames_seen)
                link = self.runner.status()
            else:
                return
        self.emit({"n": "stats", "src": self.stats_payload()})

    # ------------------------------------------------------- 契约载荷（§3.2/§3.3）
    def link_payload(self) -> dict:
        """契约 §3.1 的 `/api/link`（也是 `state` 事件里的 `link`）。

        `stats.rx/tx` = 收/发**帧数**（不是字节），`last_rx_ms` = 距上次收到车帧的**毫秒数**。
        """
        st = self.runner.status()
        with self.lock:
            fw = dict(self.hello) if self.hello else None
            hz = self.hz
        if fw is not None:
            fw = {k: fw.get(k) for k in ("name", "version", "proto", "param_crc",
                                        "telem_hz", "caps")}
        last = st.get("last_rx_ms") or 0
        age = None if not last else max(0, int(time.time() * 1000) - int(last))
        return {
            "ok": True,
            "mode": st.get("mode"),
            "port": st.get("port") or None,
            "baud": int(st.get("baud") or 115200),
            "connected": bool(st.get("open")),
            "stats": {
                "rx": int(st.get("rx_frames", 0)), "tx": int(st.get("tx_frames", 0)),
                "crc": int(st.get("crc_errors", 0)), "resync": int(st.get("resyncs", 0)),
                "drops": int(st.get("drops", 0)),
            },
            "telem_hz": hz,
            "last_rx_ms": age,
            "rtt_ms": self.rtt_ms,
            "fw": fw,
            "note": st.get("note", ""),
        }

    def stats_payload(self) -> dict:
        """契约 §4 的 `stats` 事件。

        `busy_pct` = 车上控制中断占用率（`loop_us / 5000µs`，控制周期 5ms）—— 这是桥上
        唯一能真实拿到的"负载"，不用假数字。
        """
        with self.lock:
            link = self.link_payload()
            t = self.telem or {}
            busy = 0.0
            try:
                busy = max(0.0, min(100.0, float(t.get("loop_us", 0)) / 5000.0 * 100.0))
            except Exception:
                busy = 0.0
        return {"stats": link["stats"], "telem_hz": link["telem_hz"],
                "last_rx_ms": link["last_rx_ms"], "busy_pct": round(busy, 1)}

    def record_status(self) -> dict:
        with self.lock:
            return {"ok": True, "recording": self.recording,
                    "path": self.rec_path or None, "samples": self.rec_samples,
                    "started_ms": self.rec_started}

    def params_payload(self) -> dict:
        """`/api/params` 与 SSE `params` 的载荷：前端按 `.items` 读，别改成裸列表。"""
        with self.lock:
            items = [dict(it) for it in self.params]
            crc = int((self.hello or {}).get("param_crc") or 0)
            src = "fw" if self.car_values else "local"
        return {"ok": True, "param_crc": crc, "source": src, "items": items}

    def actions_payload(self) -> dict:
        """`/api/actions` 与 SSE `actions` 的载荷（契约 §3.3）。"""
        with self.lock:
            items = [dict(a) for a in self.actions]
        return {"ok": True, "items": items}

    # ------------------------------------------------------------------ 快照
    def snapshot(self) -> dict:
        with self.lock:
            return {
                "now": int(time.time() * 1000),
                "uptime_s": int(time.time() - self.t0),
                "link": self.link_payload(),
                "hello": self.hello,
                "telem": self.telem,
                "telem_host_ms": self.telem_host_ms,
                "params": self.params_payload(),
                "actions": self.actions_payload(),
                "cal": self.cal,
                "servo": self.servo,
                "drive": self.drive_report,
                "stats": self.stats,
                "hz": self.hz,
                "telem_mode": self.telem_mode,
                "recording": self.recording,
                "rec_path": self.rec_path,
                "presets": self.preset_list(),
                "logs": list(self.logs),
                "names": {"mode": MODE_NAMES, "renwu": RENWU_NAMES, "nvram": NVRAM_NAMES},
            }

    # ------------------------------------------------------------------ 录制
    def record_start(self, path: str = "") -> str:
        os.makedirs(self.log_dir, exist_ok=True)
        if not path:
            path = os.path.join(self.log_dir, time.strftime("rec_%Y%m%d_%H%M%S.jsonl"))
        with self.lock:
            if self._rec_fh is not None:
                self._rec_fh.close()
            self._rec_fh = open(path, "w", encoding="utf-8")
            self.recording = True
            self.rec_path = path
            self.rec_samples = 0
            self.rec_started = int(time.time() * 1000)
        self.log("info", f"开始录制 → {path}")
        return path

    def record_stop(self) -> str:
        with self.lock:
            self.recording = False
            if self._rec_fh is not None:
                self._rec_fh.close()
                self._rec_fh = None
            p = self.rec_path
            n = self.rec_samples
        self.log("info", f"停止录制（{p}，{n} 帧）")
        return p

    def log_path(self) -> str:
        return os.path.join(self.log_dir, time.strftime("console_%Y%m%d.log"))

    def log_event(self, ev: dict):
        """把日志事件落到文本文件（README 里说明日志位置）。"""
        try:
            with open(self.log_path(), "a", encoding="utf-8") as fh:
                fh.write(json.dumps(ev, ensure_ascii=False) + "\n")
        except Exception:
            pass

    # ------------------------------------------------------------------ 预设
    def preset_list(self) -> list[dict]:
        out = []
        if not os.path.isdir(self.preset_dir):
            return out
        for name in sorted(os.listdir(self.preset_dir)):
            if not name.endswith(".json"):
                continue
            try:
                with open(os.path.join(self.preset_dir, name), "r", encoding="utf-8") as fh:
                    o = json.load(fh)
                o.setdefault("name", name[:-5])
                out.append(o)
            except Exception:
                continue
        return out

    def preset_save(self, name: str, kind: str, keys=None, note: str = "",
                    values: dict | None = None) -> dict:
        os.makedirs(self.preset_dir, exist_ok=True)
        o = {"name": name, "kind": kind, "updated_ms": int(time.time() * 1000),
             "keys": list(keys or []), "note": note, "values": values or {}}
        path = os.path.join(self.preset_dir, name + ".json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(o, fh, ensure_ascii=False, indent=2)
        self.log("info", f"保存预设「{name}」（{kind}，{len(o['keys'])} 项）")
        self.emit({"n": "presets", "src": {"ok": True, "items": self.preset_list()}})
        return o

    def preset_delete(self, name: str) -> dict:
        path = os.path.join(self.preset_dir, name + ".json")
        if not os.path.isfile(path):
            return {"ok": False, "error": f"没有预设 {name}"}
        try:
            os.remove(path)
        except OSError as e:
            return {"ok": False, "error": f"删不掉 {name}：{e}"}
        self.log("info", f"删除预设「{name}」")
        self.emit({"n": "presets", "src": {"ok": True, "items": self.preset_list()}})
        return {"ok": True, "name": name}

    def preset_apply(self, name: str, targets=None) -> dict:
        """应用预设；`targets` 给了就只下发这几个目标（actions/params/servo）。"""
        path = os.path.join(self.preset_dir, name + ".json")
        if not os.path.isfile(path):
            return {"ok": False, "error": f"没有预设 {name}"}
        with open(path, "r", encoding="utf-8") as fh:
            o = json.load(fh)
        kind = o.get("kind")
        want = set(targets) if targets else set()
        res = {"ok": True, "name": name, "kind": kind, "sent": 0, "results": []}
        vals = o.get("values", {})

        def pick(t: str) -> bool:
            if want:
                return t in want
            return kind in (t, "mixed")

        if pick("params"):
            pkeys = []
            for k, v in vals.get("params", {}).items():
                r = self.param_set(k, v)              # 先只改桥这边缓存
                res["results"].append(r)
                if r.get("ok"):
                    pkeys.append(k)
            if pkeys:                                 # 再一起下发到车
                r = self.param_push(keys=pkeys)
                res["results"].append(r)
                res["sent"] += int(r.get("sent") or 0)
        if pick("actions"):
            for n, a in vals.get("actions", {}).items():
                r = self.action_set(int(n), a[0], a[1])
                res["results"].append(r)
                res["sent"] += 1 if r.get("ok") else 0
        if pick("servo"):
            for ch, deg in vals.get("servo", {}).items():
                r = self.servo_set(int(ch), int(deg))
                res["results"].append(r)
                res["sent"] += 1 if r.get("ok") else 0
        res["ok"] = all(r.get("ok") for r in res["results"]) if res["results"] else False
        self.log("info", f"应用预设「{name}」：{res['sent']} 项下发，"
                         f"{'全部成功' if res['ok'] else '有失败'}")
        return res

    # ------------------------------------------------------------------ 业务动作
    def _car_value(self, sp) -> float:
        """车上的当前值；还没握手/reload 过就用出厂默认。`dirty`= 本机值 ≠ 它。"""
        return self.car_values.get(sp.id, sp.default)

    def _want_params(self, ids=None, keys=None, only_dirty=True) -> list:
        """挑出要动的参数：给了 ids/keys 就按它，否则 only_dirty 决定是否只挑脏的。"""
        sel = set()
        for x in (ids or []):
            sp = P.by_id_or_key(x)
            if sp is not None:
                sel.add(sp.id)
        for x in (keys or []):
            sp = P.by_id_or_key(x)
            if sp is not None:
                sel.add(sp.id)
        given = ids is not None or keys is not None
        out = []
        with self.lock:
            for it in self.params:
                sp = P.BY_ID.get(it["id"])
                if sp is None:
                    continue
                if given:
                    if sp.id in sel:
                        out.append((sp, it["value"]))
                elif (not only_dirty) or it["dirty"]:
                    out.append((sp, it["value"]))
        return out

    def param_set(self, id_or_key, value) -> dict:
        """只改桥这边缓存（契约 §2：不自动下发），`push` 才到车。"""
        sp = P.by_id_or_key(id_or_key)
        if sp is None:
            return {"ok": False, "error": f"没有这个参数：{id_or_key}"}
        v = P.clamp(sp, float(value))
        with self.lock:
            it = next((i for i in self.params if i["id"] == sp.id), None)
            if it is None:
                return {"ok": False, "error": f"参数表里没有 {sp.key}"}
            it["value"] = v
            it["dirty"] = bool(v != self._car_value(sp))
            dirty = it["dirty"]
        self.emit({"n": "params", "src": self.params_payload()})
        return {"ok": True, "id": sp.id, "key": sp.key, "value": v,
                "dirty": dirty, "pushed": False}

    def param_push(self, ids=None, keys=None) -> dict:
        """把本机缓存里的值逐条下发到车（不给 ids/keys 就下发所有脏的）。"""
        want = self._want_params(ids, keys)
        results, sent = [], 0
        for sp, v in want:
            r = self.send_wait(pr.T_PARAM_SET, pr.pack_u16(sp.id) + pr.pack_f32(v),
                               want=(pr.T_ACK, pr.T_PARAM_REPORT))
            ok = bool(r.get("ok") and r.get("answered"))
            if ok:
                with self.lock:
                    self._set_car_value(sp.id, v)
                sent += 1
            results.append({"id": sp.id, "key": sp.key, "value": v, "ok": ok,
                            "status": r.get("status"), "status_name": r.get("status_name")})
        with self.lock:
            left = [i["key"] for i in self.params if i["dirty"]]
        self.emit({"n": "params", "src": self.params_payload()})
        self.log("info", f"下发参数：{sent}/{len(want)} 条到车，还脏 {len(left)} 条")
        return {"ok": sent == len(want), "sent": sent, "of": len(want),
                "dirty_left": left, "results": results}

    def param_reload(self) -> dict:
        """从车读回全表（0xFFFF dump：车每拍发 4 帧，所以要等一会儿）。"""
        n0 = len(self.car_values)
        seq = self.send(pr.T_PARAM_GET, pr.pack_u16(0xFFFF))
        deadline = time.time() + 1.2
        got = 0
        while time.time() < deadline:
            time.sleep(0.05)
            with self.lock:
                got = len(self.car_values)
            if got >= len(P.PARAMS):
                break
        with self.lock:
            items = [dict(it) for it in self.params]
        self.emit({"n": "params", "src": self.params_payload()})
        self.log("info", f"从车读回参数：{got}/{len(P.PARAMS)}（读回前有 {n0} 条）")
        return {"ok": True, "seq": seq, "read": got, "of": len(P.PARAMS), "params": items}

    def param_defaults(self, ids=None, keys=None) -> dict:
        """恢复出厂默认：只改本机表 + 标脏，等 `push` 下发（契约 §2）。"""
        want = self._want_params(ids, keys, only_dirty=False)
        with self.lock:
            for sp, _ in want:
                it = next((i for i in self.params if i["id"] == sp.id), None)
                if it is None:
                    continue
                it["value"] = P.clamp(sp, sp.default)
                it["dirty"] = bool(it["value"] != self._car_value(sp))
        self.emit({"n": "params", "src": self.params_payload()})
        self.log("info", f"恢复出厂默认（仅本机表）：{len(want)} 项，等下发")
        return {"ok": True, "count": len(want), "pushed": False,
                "keys": [sp.key for sp, _ in want]}

    def param_save(self) -> dict:
        r = self.send_wait(pr.T_PARAM_SAVE, b"", want=(pr.T_ACK, pr.T_TEXT))
        return r

    def action_set(self, n: int, a1: int, a2: int) -> dict:
        r = self.send_wait(pr.T_ACTION_SET,
                           bytes([n & 0xFF, a1 & 0xFF, a2 & 0xFF]),
                           want=(pr.T_ACK, pr.T_ACTIONS))
        r.update({"n": n, "a1": a1, "a2": a2})
        if r.get("ok"):
            with self.lock:
                if 1 <= int(n) <= P.ACTION_COUNT:
                    self.action_vals[int(n) - 1] = [int(a1), int(a2)]
                self._rebuild_actions()
        return r

    def action_save(self) -> dict:
        """把当前动作角度固化：车上 RAM 已经改了，这一步（1）写进小车 Flash（2）存本机预设。"""
        r = self.send_wait(pr.T_ACTION_SAVE, b"", want=(pr.T_ACK, pr.T_TEXT))
        if r.get("ok"):
            with self.lock:
                self.action_preset = [list(v) for v in self.action_vals]
                self._rebuild_actions()
                vals = [list(v) for v in self.action_vals]
            path = self._save_action_preset(vals)
            r["preset_file"] = path
        return r

    def action_play(self, n: int) -> dict:
        r = self.send_wait(pr.T_SERVO_PLAY, bytes([n & 0xFF]), req="actions.play")
        return r

    def action_reset(self) -> dict:
        """恢复出厂动作表（只改 RAM，要用「保存」才固化）。"""
        out = []
        for n in range(1, P.ACTION_COUNT + 1):
            a1, a2 = P.ACTION_FACTORY[n - 1]
            out.append(self.action_set(n, a1, a2))
        return {"ok": all(o.get("ok") for o in out), "count": len(out), "results": out}

    def action_refresh(self) -> dict:
        seq = self.send(pr.T_ACTIONS_GET)
        t0 = time.time()
        while time.time() - t0 < 0.5:
            with self.lock:
                if self.action_vals:
                    return {"ok": True, "seq": seq, "actions": self.actions}
            time.sleep(0.02)
        return {"ok": False, "seq": seq, "error": "没收到动作表"}

    def _rebuild_actions(self):
        """（锁内调用）把车上值 + 本机预设拼成契约项。"""
        self.actions = P.action_items(self.action_vals, self.action_preset)

    def _action_preset_path(self) -> str:
        return os.path.join(self.preset_dir, "actions.json")

    def _load_action_preset(self):
        """本机动作预设：没有文件就当固件出厂表。"""
        path = self._action_preset_path()
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            vals = data.get("actions") if isinstance(data, dict) else data
            if isinstance(vals, list) and len(vals) == P.ACTION_COUNT:
                return [[int(v[0]), int(v[1])] for v in vals]
        except (OSError, ValueError, TypeError, KeyError, IndexError):
            pass
        return [list(v) for v in P.ACTION_FACTORY]

    def _save_action_preset(self, vals) -> str:
        path = self._action_preset_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump({"actions": [[int(v[0]), int(v[1])] for v in vals],
                       "updated_ms": int(time.time() * 1000)}, fh,
                      ensure_ascii=False, indent=1)
        os.replace(tmp, path)
        return path

    def servo_set(self, ch: int, deg: int) -> dict:
        r = self.send_wait(pr.T_SERVO_SET, bytes([ch & 0xFF]) + pr.pack_i16(int(deg)))
        r.update({"ch": ch, "deg": int(deg)})
        if r.get("ok"):
            with self.lock:
                self.servo[f"s{ch}_deg"] = int(deg)
        return r

    def servo_center(self) -> dict:
        return self.send_wait(pr.T_SERVO_PLAY, bytes([0]), req="servo.center")

    def servo_deviation(self, ch: int, us: int) -> dict:
        r = self.send_wait(pr.T_SERVO_DEV, bytes([ch & 0xFF]) + pr.pack_i16(int(us)))
        r.update({"ch": ch, "us": int(us)})
        if r.get("ok"):
            with self.lock:
                self.servo[f"dev{ch}_us"] = int(us)
        return r

    def drive(self, mode, vl=None, vr=None, base=None, turn=None) -> dict:
        if isinstance(mode, str):
            mode = DRIVE_MODES.get(mode, -1)
        if mode is None or int(mode) not in (0, 1, 2, 3):
            return {"ok": False, "error": f"驾驶模式不对：{mode}"}
        mode = int(mode)
        a = int(vl or 0) if mode == 1 else int(base or 0)
        b = int(vr or 0) if mode == 1 else int(turn or 0)
        r = self.send_wait(pr.T_DRIVE, bytes([mode]) + pr.pack_i16(a) + pr.pack_i16(b),
                           want=(pr.T_ACK, pr.T_DRIVE_REPORT))
        r.update({"mode": mode, "a": a, "b": b})
        now = self._now_ms()
        with self.lock:
            if r.get("ok") and mode != 0:
                self.drive_cmd = {"mode": mode, "a": a, "b": b}
                # 上一帧遥测可能早于本条指令到达（拿不到 a/b）⇒ 立刻补上，别让前端看见闪烁
                self.drive_report = dict(self.drive_report, **self.drive_cmd)
                self.drive_hold_until = now + DRIVE_HOLD_MS      # 有订阅者就会自动续期
                self.drive_ka_ms = now
            else:
                self.drive_cmd = {}
                self.drive_report = {k: v for k, v in self.drive_report.items()
                                     if k not in ("a", "b")}
                self.drive_hold_until = 0
                self.drive_ka_ms = 0
        return r

    def drive_query(self) -> dict:
        r = self.send_wait(pr.T_DRIVE_REPORT, b"", want=(pr.T_DRIVE_REPORT,))
        return r

    def cmd(self, name_or_code, arg: int = 0) -> dict:
        code = CMD_NAMES.get(name_or_code, name_or_code)
        if not isinstance(code, int):
            return {"ok": False, "error": f"没有这个命令：{name_or_code}"}
        r = self.send_wait(pr.T_CMD, bytes([code & 0xFF, int(arg) & 0xFF]),
                           want=(pr.T_ACK, pr.T_TEXT))
        r.update({"cmd": code, "arg": int(arg)})
        if r.get("ok") and code == pr.CMD_SET_TELEM_HZ and arg:
            with self.lock:
                self.hz = int(arg)
        return r

    def cal_refresh(self) -> dict:
        seq = self.send(pr.T_CAL_GET)
        t0 = time.time()
        while time.time() - t0 < 0.8:
            with self.lock:
                if self.cal:
                    return {"ok": True, "seq": seq, "cal": self.cal}
            time.sleep(0.03)
        return {"ok": False, "seq": seq, "error": "没收到标定数据"}

    def hello_req(self, timeout: float = 0.6) -> dict:
        """主动握手（连接后 / 定时握手用；拿到 caps 才知道车上支持哪些新帧）。"""
        r = self.send_wait(pr.T_HELLO_REQ, b"", want=(pr.T_HELLO,), timeout=timeout)
        with self.lock:
            h = dict(self.hello) if self.hello else {}
        r["ok"] = bool(h.get("proto"))            # HELLO 是问答式，车不回 ACK
        r["hello"] = h
        if r["ok"]:
            caps = int(h.get("caps", 0))
            if caps & 0x02:                       # 有能力位才敢要动作表
                self.action_refresh()
            if caps & 0x04:
                self.servo_dev_refresh()
        return r

    def servo_dev_refresh(self) -> dict:
        """读一次两路舵机中位偏差（TELEM2 里本来就在发，这里只是保证有值）。"""
        with self.lock:
            return {"ok": True, "dev1_us": self.servo.get("dev1_us", 0),
                    "dev2_us": self.servo.get("dev2_us", 0)}

    # ------------------------------------------------------------------ 链路切换
    def link_status(self) -> dict:
        return self.runner.status()

    def params_meta(self) -> dict:
        return {
            "groups": P.GROUPS,
            "items": [{"id": sp.id, "key": sp.key, "label": sp.label, "group": sp.group,
                       "unit": sp.unit, "default": sp.default, "min": sp.min,
                       "max": sp.max, "decimals": sp.decimals, "step": sp.step,
                       "note": sp.note} for sp in P.PARAMS],
        }
