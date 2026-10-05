"""控制台状态机：链路上的收发、参数/动作/标定缓存、日志、SSE 广播。

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
    # start_a/start_b/start_c 的字节定义保留（协议不删），但固件里的内置任务流程已删除，
    # 这三个命令现在一律回 ST_UNSUPPORTED（0x08）。「开始/停止任务」走 /api/tasks/run|stop。
    "stop": pr.CMD_STOP, "start_a": pr.CMD_START_A, "start_b": pr.CMD_START_B,
    "start_c": pr.CMD_START_C, "calib_gyro": pr.CMD_CALIB_GYRO,
    "save_params": pr.CMD_SAVE_PARAMS, "clear_fault": pr.CMD_CLEAR_FAULT,
    "set_telem_hz": pr.CMD_SET_TELEM_HZ, "reset": pr.CMD_RESET,
    "set_logging": pr.CMD_SET_LOGGING, "set_telem_mode": pr.CMD_SET_TELEM_MODE,
}
DRIVE_MODES = {"off": 0, "wheels": 1, "base_turn": 2, "trace": 3}
DRIVE_KEEPALIVE_MS = 200      # 车上驾驶看门狗是 500ms，留 2 倍余量
DRIVE_HOLD_MS = 3000          # 有 /api/stream 订阅者挂着就算"在场"，超时就不再喂
MODE_NAMES = {0: "空闲", 1: "陀螺", 2: "循迹", 3: "串级"}
NVRAM_NAMES = {0: "空闲", 1: "待保存", 2: "保存中", 3: "已保存", 4: "失败"}
ST_NAMES = {0x00: "OK", 0x02: "未知类型", 0x03: "参数错", 0x04: "超范围",
            0x05: "未就绪", 0x06: "命令错", 0x07: "忙", 0x08: "不支持"}
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
        # 车上任务程序（docs/任务编程.md §5）：目录只在握手后请求一次，状态由车推
        self.tasks: list[dict] = []
        self.task_status: dict | None = None
        self._task_list_items: list[dict] = []
        self._task_list_done = False
        self._task_data_chunks: list[tuple[int, bytes]] = []
        self._task_data_done = False
        self._task_data_id = 0xFFFF
        # GET 的应答状态码：读回**成功时不回 ACK**，所以只有失败才看得到它
        self._task_get_status: int | None = None
        self.logs: deque = deque(maxlen=LOG_MAX)
        self.replies: dict[int, list[pr.Frame]] = {}
        self.subs: list[Subscriber] = []
        self.hz = 50
        self.telem_mode = 1
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
        # **实测**帧率（每 1s 由 _maybe_stats 数出来）。别和 `self.hz` 混：那是车上固件
        # 报上来的**设定值**（T_HELLO 的 telem_hz / link_payload 的 telem_hz）。
        self.measured_hz = 0
        self.measured_tx_hz = 0
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

    def _link_alive(self, max_age_ms: int = 500) -> bool:
        """链路还在来帧吗（车没掉线）。用来决定"无应答"值不值得重发：
        车要是彻底不说话了，重发只是白等一遍超时。"""
        try:
            st = self.runner.status()
        except Exception:
            return False
        if not st.get("open", False):
            return False
        age = self._now_ms() - int(st.get("last_rx_ms") or 0)
        return 0 <= age <= max_age_ms

    def send_wait(self, type_: int, payload: bytes = b"", want=(pr.T_ACK,),
                  timeout: float = 0.6, req: str | None = None,
                  retries: int = 0) -> dict:
        """发一帧并等应答，**"一帧都没回来"就换新 seq 重发**（最多 `retries` 次）。

        `retries` 的安全边界（很关键，别乱传）：
          · 只有这条 seq **一帧都没回来**（`answered` 为假）才重发——收到明确拒绝
            （status != ST_OK）绝不重发；
          · 只对**幂等**请求用：设参数、drive 设定点、读全表、握手；
          · `T_CMD` 的复位/陀螺标定、`T_ACTION_PLAY` / `T_SERVO_PLAY` 这类**一次性动作
            必须留 0**，否则会重放一次。
        为什么需要它：车的 host-link 发是 8 槽有损队列（`firmware/Core/Src/board.c`
        的 `s_dl_tx_drop`），队列满就**静默丢帧**，丢的往往正是 ACK —— 车其实已经执行了，
        但上位机只看到"无应答"。上位机自己的驱动接收缓冲也有个 4096 B 的老默认
        （见 `link.py` 的 `RX_BUFFER` 注释）。重发幂等请求能同时盖住这两种丢帧。
        """
        r = self._send_wait_once(type_, payload, want, timeout, req)
        n, lim = 0, max(0, int(retries))
        while (not r["answered"]) and n < lim and self._link_alive():
            n += 1
            r = self._send_wait_once(type_, payload, want, timeout, req)
        if n:
            r["retries"] = n
            if r["answered"]:
                self.log("info", f"{req or 'req'}：前 {n} 次无应答，第 {n + 1} 次才回")
            elif r.get("error"):
                r["error"] = f"{r['error']}（已重发 {n} 次）"
        return r

    def _send_wait_once(self, type_: int, payload: bytes = b"", want=(pr.T_ACK,),
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
        ok = (st == pr.ST_OK) if st is not None else False
        name = ST_NAMES.get(st, "无应答" if st is None else f"未知({st:#04x})")
        out = {
            "seq": seq, "sent": True,
            "status": st if st is not None else None,
            "status_name": name,
            "ok": ok,
            "answered": bool(got),
            "frames": [{"type": f.type, "payload_hex": f.payload.hex()} for f in got],
        }
        if not ok:
            # 语义性失败也必须带一句**人话**。
            # 前端 api.ts 在 `{ok:false}` 且没有 error 时会退到"请求失败（HTTP xxx）"，
            # 而本桥的语义失败一律回 HTTP 200 ⇒ 用户看到的就是「请求失败（HTTP 200）」
            # （用户 m24215 报的"什么 HTTP002。200"就是这个）。这里补上真正的原因。
            if st is None:
                # 别写成"车没收到指令"：车的 host-link 发是 8 槽有损队列
                # （firmware/Core/Src/board.c 的 s_dl_tx_drop），队列满就静默丢帧，
                # 丢的往往正是这条 ACK —— **车多半已经执行了**。上位机的驱动接收
                # 缓冲也还有个 4096B 的老默认（见 link.py 的 RX_BUFFER）。
                err = (f"车上没应答（等 {timeout:.1f}s 超时；多半只是这一帧丢了，"
                       f"车可能已经执行）" if not got
                       else f"车上没回确认帧（收到 {len(got)} 帧，都不是 ACK）")
            else:
                err = f"车上拒绝了：{name}"
            out["error"] = err
            self.log("warn", f"{req or 'req'} 失败：{err}（seq={seq}）")
        return out

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
                if req == "tasks.get":
                    with self.cv:
                        self._task_get_status = st   # 读回失败才有 ACK（成功走 DATA）
                        self.cv.notify_all()
                self.emit({"n": "ack", "src": {
                    "seq": f.seq, "req": req, "status": st, "ok": (st == pr.ST_OK),
                    "text": ST_NAMES.get(st, f"{st:#04x}"),
                    "status_name": ST_NAMES.get(st, f"{st:#04x}")}})
        elif f.type == pr.T_TEXT:
            self.log("info", "[车] " + pr.decode_text(f.payload))
        elif f.type == pr.T_TASK_LIST_R:
            it = pr.decode_task_list_r(f.payload)
            with self.cv:
                if it is None:
                    self._task_list_done = True        # 空载荷 = 目录发完
                else:
                    self._task_list_items.append(it)
                self.cv.notify_all()
        elif f.type == pr.T_TASK_DATA:
            tid, off, data = pr.decode_task_data(f.payload)
            with self.cv:
                if data:
                    self._task_data_chunks.append((off, data))
                else:
                    self._task_data_done = True        # 没有数据 = 读完
                    self._task_data_id = tid
                self.cv.notify_all()
        elif f.type == pr.T_TASK_STATUS:
            st = pr.decode_task_status(f.payload)
            with self.lock:
                self.task_status = st
            self.emit({"n": "task", "src": st})
        # 其它帧不再单发 SSE 事件：以前这里给每帧推一条 {"n":"frame"}，
        # 前端从来没有订阅过，纯粹是白占带宽（帧数在 stats 里看得到）。

    def _maybe_stats(self):
        now = time.time()
        with self.lock:
            if now - self._rate_t >= 1.0:
                dt = max(1e-3, now - self._rate_t)
                self._rate = {k: int(v / dt) for k, v in self._rate_n.items()}
                self._rate_n = {k: 0 for k in self._rate_n}
                self._rate_t = now
                # 实测帧率只在这个 1s 窗口里算得出来，**必须存下来给 stats 事件用**。
                # 2026-10-05 修（用户 m30228「不是还换了 200Hz 吗，为什么还是 100Hz」）：
                # 原来这里把实测值算进一个本地 dict 就丢了，stats 事件回的还是固件
                # 上报的**设定值** ⇒ 车上设定 200Hz，界面也永远显示 200（或调之前的值），
                # 从来没人知道链路真正跑出来是多少。
                self.measured_hz = self._rate["telem"]
                self.measured_tx_hz = self._rate["tx"]
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
            "baud": int(st.get("baud") or 921600),
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

        两个帧率**别混**：
          · `telem_hz`      = **实测**（1s 窗口数出来的帧率）—— 界面显示帧率用这个；
          · `telem_hz_set`  = 车上固件报的**设定值**（和 `/api/link` 的 `telem_hz` 同源）。
        2026-10-05 修（用户 m30228「不是还换了 200Hz 吗，为什么还是 100Hz」）：原来这里回的是
        设定值，实测值算完就丢 ⇒ 界面上"设成 200 还是显示 100/之前的值"，永远看不出链路
        真实吞吐（921600 下 100Hz 只占约 8% 带宽、200Hz 约 15%，到底跑没跑得动只能看实测）。

        `busy_pct` = 车上**主循环里界面刷新**（`app_ui_tick`）耗时占 5ms 控制拍的比例，
        源自固件遥测 `loop_us`（`host_link.c:465`）。它**不是**控制中断占用 —— 实测控制中断
        只有 ~9µs（RTT `ISR: avg=9 us`），而界面刷新平均 834µs、峰值 4.3ms，所以这个数会
        在 0.2% 和 81% 之间跳。"""
        with self.lock:
            link = self.link_payload()
            t = self.telem or {}
            busy = 0.0
            try:
                busy = max(0.0, min(100.0, float(t.get("loop_us", 0)) / 5000.0 * 100.0))
                loop_us = max(0, int(t.get("loop_us", 0) or 0))
            except Exception:
                busy = 0.0
                loop_us = 0
            measured = int(self.measured_hz)
            measured_tx = int(self.measured_tx_hz)
        return {"stats": link["stats"], "telem_hz": measured, "telem_hz_set": link["telem_hz"],
                "telem_tx_hz": measured_tx, "loop_us": loop_us,
                "last_rx_ms": link["last_rx_ms"], "busy_pct": round(busy, 1)}

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

    # ------------------------------------------------------------------ 任务程序
    # 帧语义见 docs/任务编程.md §5；存储/解释器在固件 App/Svc/task_store.c、task_run.c。
    # 这一层只做四件事：能力位把关、多帧收齐（目录/数据）、单帧的 send_wait、状态镜像。

    def cap_task(self) -> bool:
        """车上固件认不认任务帧 —— 看 HELLO 的 HOST_CAP_TASK(bit7)。

        还没握过手时不知道能力，先按"能"试（真发一帧就知道），别凭空把功能禁掉。
        """
        caps = (self.hello or {}).get("caps")
        if caps is None:
            return True
        return bool(int(caps) & pr.CAP_TASK)

    def unsupported(self) -> dict:
        return {"ok": False, "supported": False,
                "error": "车上固件不支持任务功能（HOST_CAP_TASK 未置位）—— 先升级固件再试"}

    def _cap_guard(self) -> dict | None:
        return None if self.cap_task() else self.unsupported()

    def _wait_flag(self, attr: str, timeout: float) -> bool:
        """等 on_frame 里的某个"收齐"标记（多帧回包用，不能只等一帧）。"""
        deadline = time.time() + max(0.05, timeout)
        with self.cv:
            while time.time() < deadline:
                if getattr(self, attr):
                    return True
                self.cv.wait(max(0.01, deadline - time.time()))
            return bool(getattr(self, attr))

    def task_list(self, timeout: float = 1.5) -> dict:
        """`T_TASK_LIST` → 车上一条任务一帧地回 `T_TASK_LIST_R`，空载荷收尾。"""
        g = self._cap_guard()
        if g:
            return g
        with self.cv:
            self._task_list_items = []
            self._task_list_done = False
        self.send(pr.T_TASK_LIST, b"", req="tasks.list")
        done = self._wait_flag("_task_list_done", timeout)
        with self.cv:
            items = [dict(it) for it in self._task_list_items]
        if not done:
            return {"ok": False, "supported": True, "items": items,
                    "error": "没等到目录收尾帧（车没回或帧丢）—— 再试一次"}
        with self.lock:
            self.tasks = items
        self.emit({"n": "tasks", "src": {"ok": True, "items": items}})
        return {"ok": True, "supported": True, "items": items}

    def task_get(self, tid: int, timeout: float = 2.0) -> dict:
        """`T_TASK_GET` → 一串 `T_TASK_DATA`（成功时不回 ACK），按 off 拼回整镜像。"""
        g = self._cap_guard()
        if g:
            return g
        with self.cv:
            self._task_data_chunks = []
            self._task_data_done = False
            self._task_data_id = 0xFFFF
            self._task_get_status = None
        self.send(pr.T_TASK_GET, pr.pack_u16(tid), req="tasks.get")
        done = self._wait_flag("_task_data_done", timeout)
        with self.cv:
            chunks = list(self._task_data_chunks)
            got_id = self._task_data_id
            ack = self._task_get_status
        if not done:
            if ack == pr.ST_NOT_READY:
                return {"ok": False, "supported": True,
                        "error": f"车上没有任务 {tid}（槽号是空的）"}
            if ack is not None:
                return {"ok": False, "supported": True,
                        "error": f"车上拒绝读回任务 {tid}：{ST_NAMES.get(ack, ack)}（status={ack}）"}
            return {"ok": False, "supported": True,
                    "error": "没等到数据收尾帧（车没回或帧丢）—— 再试一次"}
        buf = bytearray()
        for off, data in sorted(chunks):
            if off != len(buf):
                return {"ok": False, "supported": True,
                        "error": f"数据块不连续（off={off}，已有 {len(buf)} B）"}
            buf += data
        info = next((it for it in self.tasks if it["id"] == tid), None)
        return {"ok": True, "supported": True, "id": got_id, "hex": bytes(buf).hex(),
                "len": len(buf),
                "crc": pr.crc16_ccitt(bytes(buf[12:])) if len(buf) >= 12 else 0,
                "name": (info or {}).get("name", "")}

    def _task_reply(self, r: dict, what: str) -> dict:
        """`send_wait()` 的结果 → 前端契约（必须带 ok/error/supported）。

        `send_wait` 现在自己就会给失败补一句基础的 `error`（"没应答"/"被拒绝"，
        见那边的注释：不补的话页面只剩一句 "HTTP 200"）；这里再把"没人应答"按链路实况
        说细一点（用户 2026-10-04 报的提示就是混成了一句话）：
        链路没开 / 链路开着但收不到车帧 / 链路正常在收帧却没这条 ACK。
        """
        if r.get("ok"):
            return {**r, "supported": True}
        if not r.get("answered"):
            link = self.link_payload()
            age = link.get("last_rx_ms")
            if not link.get("connected"):
                why = "链路没打开（车没连上/没上电）"
            elif age is None or age > 1500:
                why = f"链路开着但 {age if age is not None else '一直'}ms 没收到车的帧（车没在发）"
            else:
                why = ("链路正常（刚 {0}ms 前还收到车帧），但这条没等到 ACK —— "
                       "多半是固件不带任务功能（HELLO 的 HOST_CAP_TASK 未置位），"
                       "也可能只是这一帧丢了，再试一次").format(age)
            return {**r, "ok": False, "supported": True, "error": f"{what}没人应答：{why}"}
        return {**r, "ok": False, "supported": True,
                "error": f"{what}被车拒绝：{r.get('status_name', '?')}（status={r.get('status')}）"}

    def task_put(self, tid: int, off: int, data: bytes) -> dict:
        g = self._cap_guard()
        if g:
            return g
        if len(data) > pr.TASK_PUT_MAX:
            return {"ok": False, "supported": True,
                    "error": f"一片最多 {pr.TASK_PUT_MAX} 字节（收到 {len(data)}）"}
        return self._task_reply(
            self.send_wait(pr.T_TASK_PUT, pr.pack_u16(tid) + pr.pack_u16(off) + bytes(data),
                           req="tasks.put"), "写分片")

    def task_commit(self, tid: int, length: int, crc: int, name: str = "",
                    flags: int = 0) -> dict:
        g = self._cap_guard()
        if g:
            return g
        return self._task_reply(
            self.send_wait(pr.T_TASK_COMMIT,
                           pr.payload_task_commit(tid, length, crc, name, flags),
                           timeout=1.2, req="tasks.commit"), "提交任务")

    def task_delete(self, tid: int) -> dict:
        g = self._cap_guard()
        if g:
            return g
        return self._task_reply(self.send_wait(pr.T_TASK_DEL, pr.pack_u16(tid), timeout=1.2,
                                               req="tasks.delete"), "删除任务")

    def task_run(self, tid: int) -> dict:
        g = self._cap_guard()
        if g:
            return g
        return self._task_reply(self.send_wait(pr.T_TASK_RUN, pr.pack_u16(tid), timeout=1.5,
                                               req="tasks.run"), "启动任务")

    def task_stop(self) -> dict:
        g = self._cap_guard()
        if g:
            return g
        return self._task_reply(self.send_wait(pr.T_TASK_STOP, b"", timeout=1.2, req="tasks.stop"),
                                "停止任务")

    def task_status_payload(self) -> dict:
        """`/api/tasks/status`：车一有变化就推、没变 1Hz 心跳，所以这里回镜像即可。"""
        with self.lock:
            st = dict(self.task_status) if self.task_status else None
        return {"ok": True, "supported": self.cap_task(), "status": st}

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
                # 车上任务目录与运行状态镜像（docs/任务编程.md §5）：让 /api/state 与
                # SSE 首帧也能把 #/tasks 页恢复成刷新前的样子，不用再等一轮 LIST 往返。
                "tasks": [dict(t) for t in self.tasks],
                "task_status": dict(self.task_status) if self.task_status else None,
                "cal": self.cal,
                "servo": self.servo,
                "drive": self.drive_report,
                "stats": self.stats,
                "hz": self.hz,
                "telem_mode": self.telem_mode,
                "presets": self.preset_list(),
                "logs": list(self.logs),
                "names": {"mode": MODE_NAMES, "nvram": NVRAM_NAMES},
            }

    # ------------------------------------------------------------------ 日志文件
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
    @staticmethod
    def _normalize_preset(o) -> dict:
        """把磁盘上的预设文件归一成契约形状（`tools/console/docs/API.md` 的 PresetItem）。

        `preset_dir` 里住着两种 json：`preset_save()` 写的 {name,kind,keys,values}，
        以及 `_save_action_preset()` 写的动作表存档 {actions:[[a1,a2]…]}（**没有 kind/keys**）。
        旧版 `preset_list()` 把后者原样吐给前端 ⇒ 舵机页/设置页读 `p.keys.length` 抛
        TypeError、React 卸掉整棵树 ⇒ 浏览器里整页空白（2026-10-03 实测）。
        这里按内容把缺的 kind/keys 推断出来，并把顶层 `actions` 列表转成
        `values.actions = {"1": [a1,a2], …}`，顺带让「应用预设」对动作表存档也能用。
        """
        o = dict(o) if isinstance(o, dict) else {}
        vals = o.get("values")
        vals = dict(vals) if isinstance(vals, dict) else {}
        acts = o.get("actions")
        if isinstance(acts, list) and not vals.get("actions"):
            pairs = {}
            for i, v in enumerate(acts):
                if isinstance(v, (list, tuple)) and len(v) >= 2:
                    try:
                        pairs[str(i + 1)] = [int(v[0]), int(v[1])]
                    except (TypeError, ValueError):
                        pass
            if pairs:
                vals["actions"] = pairs
        filled = [k for k in ("actions", "params", "servo", "cal") if vals.get(k)]
        kind = o.get("kind")
        if not isinstance(kind, str) or not kind:
            kind = filled[0] if len(filled) == 1 else "mixed"
        keys = o.get("keys")
        if not isinstance(keys, list) or not keys:
            # 空数组也要按内容补：旧版 preset_save() 不带 targets 时存出来就是空 keys，
            # 界面上会显示「0 项」（明明带着一整张参数表），比崩掉轻、但同样是错的。
            keys = filled
        upd = o.get("updated_ms")
        note = o.get("note")
        name = o.get("name")
        return {
            "name": name if isinstance(name, str) else "",
            "kind": kind,
            "updated_ms": upd if isinstance(upd, (int, float)) else 0,
            "keys": [k for k in keys if isinstance(k, str)],
            "note": note if isinstance(note, str) else "",
            "values": vals,
        }

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
            except Exception:
                continue
            item = self._normalize_preset(o)
            item["name"] = item["name"] or name[:-5]
            out.append(item)
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
        o = self._normalize_preset(o)            # 动作表存档 actions.json 也走这条路
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
                               want=(pr.T_ACK, pr.T_PARAM_REPORT),
                               req=f"params.set {sp.key}", retries=2)
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
                           want=(pr.T_ACK, pr.T_DRIVE_REPORT), req="drive", retries=1)
        r.update({"mode": mode, "a": a, "b": b})
        if not r.get("ok"):
            # 操控页是 10Hz 连续下发（转向时每 100ms 一帧），车一拒绝就会连着弹 ——
            # 所以拒绝原因要说成"下一步该怎么办"，而不是把状态码丢给用户。
            if r.get("status") == pr.ST_NOT_READY:
                r["error"] = "任务正在跑，电机被任务占着 —— 先去「任务」页停掉任务再操控"
            elif r.get("status") == pr.ST_BUSY:
                r["error"] = "车上忙（上一个动作还没做完），松手等一下再操控"
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
        r = self.send_wait(pr.T_CMD, pr.cmd_payload(code, arg),
                           want=(pr.T_ACK, pr.T_TEXT))
        r.update({"cmd": code, "arg": int(arg)})
        if r.get("ok") and code == pr.CMD_SET_TELEM_HZ and arg:
            # 与固件同口径的钳位（`host_link.c:800-803`：arg<1 → 1、arg>200 → 200）。
            # 不钳的话 `/api/link` 会报一个车上根本没在跑的速率（实测：请求 300，车跑 200，
            # 网页滑块却显示 300）。
            with self.lock:
                self.hz = max(1, min(200, int(arg)))
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
