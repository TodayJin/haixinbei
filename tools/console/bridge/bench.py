"""承载量测：客户机看得到多少帧、抖动多大、丢没丢、桥花多少 CPU、指令到"看得见"多长。

`bridge.probe` 管**线上有没有合法帧**（串口那半步），本工具管**桥→网页这一步**：
同样的桥进程，挂一路 SSE 客户端，按秒统计。

四种接法（前三种本工具自己起桥、量完就关；第四种量已经在跑的桥）：

    python -m bridge.bench --attach 8770              # 量正在跑的真车桥（最常用）
    python -m bridge.bench --serial COM26 --baud 230400
    python -m bridge.bench --sim                      # 仿真车：量"发指令 → 遥测里看见"
    python -m bridge.bench --replay --hz 50,100,200   # 定频回放：只测主机侧承载

说明白它能说明什么、不能说明什么：

- 回放/仿真**不经过 UART**，所以它们的数字只代表"桥 + SSE + 浏览器上游"这一步的承载与延迟；
  串口本身（波特率、CRC 错、重同步）看 `stats.crc` / `stats.resync` 与 `bridge.probe`。
- 唯一会让**所有**数字都动的是 Windows 计时器精度：不提高（系统默认 ~15.6ms）时
  `LinkRunner` 的 5ms 轮询实际只有 ~64 次/s，100Hz 遥测会被攒成串。桥默认会提高，
  本工具的子进程也默认提高；对照组加 `--low-res-timer`。
- `--serial` 开的是 J-Link 的 CDC VCOM 时，>230400 会被**拦住**（实测那颗探针试 460800/921600
  会把 USB 栈打进 bootloader 态，只能物理拔插）。确实要试加 `--force-baud`。

判定/汇总逻辑（`parse_sse` / `pct` / `summarize` / `format_report` / `serial_verdict`）都是纯函数，
由 `tools/console/tests/test_bench.py` 钉住；这里只有 socket / 子进程 / Windows API 是 I/O。

用法（工作目录 `tools/console`，或用绝对路径的 `python -m`）。
"""
from __future__ import annotations

import argparse
import ctypes
import glob
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

from .console_io import harden_stdio
from .serial_safety import check_baud

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE = os.path.dirname(HERE)
PY = sys.executable

# --------------------------------------------------------------------- 纯函数
def parse_sse(buf: bytes) -> tuple[list[tuple[str | None, object, int]], bytes]:
    """从字节缓冲里切出完整 SSE 报文：回 `([(事件名, 载荷对象, 报文字节数)], 剩余缓冲)`。

    只认 `event:` / `data:` 两行；`: ping` 心跳（没有 data）和解不开的 JSON 直接丢。
    报文可能被 TCP 切成任意片段，所以剩余缓冲必须原样带回去给下一次调用。
    """
    out: list[tuple[str | None, object, int]] = []
    while b"\n\n" in buf:
        raw, buf = buf.split(b"\n\n", 1)
        name: str | None = None
        data: str | None = None
        for line in raw.split(b"\n"):
            if line.startswith(b"event: "):
                name = line[7:].decode("utf-8", "replace")
            elif line.startswith(b"data: "):
                data = line[6:].decode("utf-8", "replace")
        if data is None:
            continue                                  # `: ping` 心跳
        try:
            out.append((name, json.loads(data), len(raw) + 2))
        except ValueError:
            continue
    return out, buf


def pct(vals: list[float], q: float) -> float:
    """最近秩百分位（样本少时比插值稳）。空列表回 0.0。"""
    if not vals:
        return 0.0
    v = sorted(vals)
    return v[min(len(v) - 1, max(0, int(round(q / 100.0 * (len(v) - 1)))))]


def expand(events: list[tuple[str, object, float, int]]) -> list[tuple[object, float, int]]:
    """把 `telem_batch` 摊回"一帧一条"：`[(载荷, 到达时刻, 该帧摊到的字节数)]`。

    桥默认按 `--sse-batch-ms` 攒批（连续遥测合成一条 `telem_batch` + 载荷数组），
    所以**帧率必须按载荷里的帧数算，不能按报文数算** —— 否则一开合并就报"帧率减半"。
    字节数按帧数摊（报文字节数包含 `event: …\\ndata: …\\n\\n` 的外壳，摊完单帧字节
    会略小于不合并时，这正是合并省下来的东西）。非遥测事件这里直接丢掉。
    """
    out: list[tuple[object, float, int]] = []
    for name, payload, t, nbytes in events:
        if name == "telem":
            out.append((payload, t, nbytes))
        elif name == "telem_batch" and isinstance(payload, list) and payload:
            k = len(payload)
            out += [(it, t, nbytes // k) for it in payload]
    return out


def summarize(events: list[tuple[str, object, float, int]], dt: float,
              fed_frames: int | None = None, cpu: float | None = None,
              log: str | None = None) -> dict:
    """把 `[(事件名, 载荷, 到达时刻 monotonic, 报文字节数)]` 汇总成一份可打印/可断言的字典。

    `fed_frames` 是同一时段里**桥自己记的收帧数**（`/api/link` 的 `stats.rx` 增量）：
    它减掉客户端收到的遥测条数就是"路上丢了多少"（窗口两端会各差 ±1 帧，正常）。
    `cpu` 是同一窗口内的单核占用百分比均值。
    帧数走 `expand()`（`telem_batch` 里的每帧都要算），报文数（`events`）才是 SSE 报文数。
    """
    frames = expand(events)
    times = [f[1] for f in frames]
    gaps = [(times[i + 1] - times[i]) * 1000.0 for i in range(len(times) - 1)]
    nbytes = sum(e[3] for e in events)
    kinds: dict[str, int] = {}
    for e in events:
        kinds[e[0]] = kinds.get(e[0], 0) + 1
    return {
        "seconds": dt,
        "telem": len(frames),
        "fps": (len(frames) / dt) if dt else 0.0,
        "fed": fed_frames,
        "fed_fps": (fed_frames / dt) if (fed_frames is not None and dt) else None,
        "dropped": (max(0, fed_frames - len(frames)) if fed_frames is not None else None),
        "gap_p50": pct(gaps, 50),
        "gap_p95": pct(gaps, 95),
        "gap_max": (max(gaps) if gaps else 0.0),
        "events": len(events),
        "ev_per_s": (len(events) / dt) if dt else 0.0,
        "bytes": nbytes,
        "kb_per_s": (nbytes / dt / 1024.0) if dt else 0.0,
        "bytes_per_event": nbytes / max(1, len(events)),
        "batched": kinds.get("telem_batch", 0),
        "per_event": (len(frames) / len(events)) if events else 0.0,
        "kinds": kinds,
        "cpu": cpu,
        "log": log,
    }


def format_report(rep: dict, title: str) -> list[str]:
    """把 `summarize()` 的结果排成三行给人看（数是实测的，别改口径）。"""
    fed = ""
    if rep.get("fed_fps") is not None:
        fed = "（链路喂了 %6.1f 帧/s，丢 %d 帧）" % (rep["fed_fps"], rep.get("dropped") or 0)
    line1 = "  %s → 实收遥测 %6.1f 帧/s%s" % (title, rep["fps"], fed)
    gap_tag = "（报文级）" if rep.get("batched") else ""
    line2 = ("               间隔%s p50 %5.1fms  p95 %5.1fms  max %6.1fms | "
             "SSE %6.1f 事件/s %7.1f KB/s 单条 %4.0fB"
             % (gap_tag, rep["gap_p50"], rep["gap_p95"], rep["gap_max"],
                rep["ev_per_s"], rep["kb_per_s"], rep["bytes_per_event"]))
    cpu = (("%5.1f%%（单核，窗口内均值）" % rep["cpu"]) if rep.get("cpu") is not None
           else "取不到")
    line3 = "               桥进程 CPU %s | 事件构成 %s | 日志 %s" % (
        cpu, rep["kinds"], os.path.basename(rep["log"]) if rep.get("log") else "-")
    out = [line1, line2, line3]
    single = rep["kinds"].get("telem", 0)
    if rep.get("batched"):
        msgs = single + rep["batched"]
        out.append("               SSE 合并：%d 条 telem_batch 装了 %d 帧（遥测报文平均每条 %.1f 帧，"
                   "报文数 %d → 不合并时 %d）；上面那行的间隔是**报文**到达口径"
                   "（同一条里的帧同刻到达），单帧最坏晚一个攒批窗口"
                   % (rep["batched"], rep["telem"] - single,
                      (rep["telem"] / msgs) if msgs else 0.0, msgs, rep["telem"]))
    return out


def serial_verdict(link: dict, got_frames: int) -> tuple[int, str]:
    """串口模式下量完之后判个结论：回 `(退出码, 一行说明)`。

    `link` 是 `GET /api/link` 的载荷，`got_frames` 是客户端这一时段实收的遥测条数。
    这里只做"分诊"——真要知道是波特率还是线的问题，用 `bridge.probe`。
    """
    st = link.get("stats") or {}
    if got_frames > 0:
        return 0, "链路在出帧：rx=%s tx=%s crc=%s resync=%s" % (
            st.get("rx"), st.get("tx"), st.get("crc"), st.get("resync"))
    if not link.get("connected"):
        return 2, ("桥没连上串口（connected=false）⇒ 先看口名对不对、有没有被别的程序占着")
    if int(st.get("rx") or 0) > 0:
        return 2, ("桥在收字节（rx=%s）但一帧都解不出 ⇒ 典型的**波特率不匹配**，"
                   "用 python -m bridge.probe --port %s 扫一遍" % (st.get("rx"), link.get("port")))
    return 2, ("一个字节都没收到（rx=0）⇒ 不是波特率就是线：先 bridge.probe 静听，"
               "再确认板上固件的 DL_BAUD 和桥 --baud 一致")


# ---------------------------------------------------------------- 进程 CPU（Windows）
class _FILETIME(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint32), ("hi", ctypes.c_uint32)]


def _ft(v: _FILETIME) -> int:
    return (v.hi << 32) | v.lo


def proc_cpu_s(pid: int) -> float | None:
    """进程累计 CPU 秒（内核+用户）。非 Windows / 取不到就回 None。"""
    if os.name != "nt":
        return None
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)          # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    try:
        c, e, k, u = _FILETIME(), _FILETIME(), _FILETIME(), _FILETIME()
        if not k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e),
                                   ctypes.byref(k), ctypes.byref(u)):
            return None
        return (_ft(k) + _ft(u)) / 1e7
    finally:
        k32.CloseHandle(h)


# ------------------------------------------------------------------------ HTTP
def req(port: int, path: str, body=None, timeout: float = 5.0):
    url = "http://127.0.0.1:%d%s" % (port, path)
    if body is None:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    data = json.dumps(body).encode("utf-8")
    r2 = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r2, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ------------------------------------------------------------------ SSE 客户端
class Sse:
    """最朴素的 SSE 客户端：原始 socket 收，`parse_sse` 切帧，每条记到达时刻。"""

    def __init__(self, port: int, host: str = "127.0.0.1"):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.sendall(("GET /api/stream HTTP/1.1\r\nHost: %s\r\n"
                        "Accept: text/event-stream\r\n\r\n" % host).encode())
        self.buf = b""

    def pump(self, seconds: float) -> list[tuple[str, object, float, int]]:
        out: list[tuple[str, object, float, int]] = []
        end = time.monotonic() + seconds
        self.s.settimeout(0.2)
        while True:
            if time.monotonic() >= end:
                break
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            self.buf += chunk
            evs, self.buf = parse_sse(self.buf)
            now = time.monotonic()
            for name, obj, nb in evs:
                out.append((name or "", obj, now, nb))
        return out

    def close(self) -> None:
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------- 起桥/杀桥
def spawn(extra_args: list[str], port: int, tag: str, low_res_timer: bool,
          sse_batch_ms: int | None = None):
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
    log_path = os.path.join(CONSOLE, "data", "logs", "bench_bridge_%s.log" % tag)
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    fh = open(log_path, "wb")
    cmd = [PY, "-m", "bridge.main", "--port", str(port), "--no-browser",
           *extra_args, *(["--low-res-timer"] if low_res_timer else []),
           *(["--sse-batch-ms", str(sse_batch_ms)] if sse_batch_ms is not None else [])]
    p = subprocess.Popen(cmd, cwd=CONSOLE, env=env, stdin=subprocess.DEVNULL,
                         stdout=fh, stderr=subprocess.STDOUT)
    for _ in range(120):
        try:
            req(port, "/api/health", timeout=1.0)
            return p, log_path
        except Exception:
            if p.poll() is not None:
                raise SystemExit("桥进程退出（码 %s），看 %s" % (p.returncode, log_path))
            time.sleep(0.1)
    raise SystemExit("桥没起来，看 %s" % log_path)


def kill(p) -> None:
    try:
        p.terminate()
        p.wait(timeout=5)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass


# ------------------------------------------------------------------- 量测核心
def measure(port: int, seconds: float, cpu_pid: int | None = None,
            warmup: float = 1.0, ticks: float = 0.5) -> dict:
    """挂一路 SSE 量 `seconds` 秒，返回 `(事件, 时长, 桥同期收帧数, 桥 CPU 均值)`。

    **窗口必须对齐**：`/api/link` 的 `rx` 计数在热身前后各读一次会把热身的 1 秒也算进来，
    喂帧率正好放大 (1+seconds)/seconds 倍、凭空多出"丢帧"（50Hz 档实测 63.2 帧/s 对
    50.2 帧/s = 1.26 倍，就是热身那 1 秒）。所以热身单独做，计时从热身结束那刻起，
    `rx` 与 CPU 读数都在这两刻取。

    CPU 报**窗口内均值**（`(c1-c0)/dt`）：按 0.5s 分片取中位数在同一组参数下会给出
    0.0% / 6.1% / 9.1% 三种结果，没法写进报告。
    """
    sse = Sse(port)
    try:
        sse.pump(warmup)                       # 热身：连上补发的 state + HTTP 头
        rx0 = int((req(port, "/api/link").get("stats") or {}).get("rx") or 0)
        c0 = proc_cpu_s(cpu_pid) if cpu_pid else None
        t0 = time.monotonic()
        events: list[tuple[str, object, float, int]] = []
        while True:
            left = t0 + seconds - time.monotonic()
            if left <= 0:
                break
            events += sse.pump(min(ticks, left))
        dt = time.monotonic() - t0
        rx1 = int((req(port, "/api/link").get("stats") or {}).get("rx") or 0)
        c1 = proc_cpu_s(cpu_pid) if cpu_pid else None
    finally:
        sse.close()
    cpu = (100.0 * (c1 - c0) / dt) if (c0 is not None and c1 is not None and dt > 0) else None
    return {"events": events, "dt": dt, "fed": max(0, rx1 - rx0), "cpu": cpu}


def bench_attached(port: int, seconds: float) -> dict:
    """量一个**已经在跑**的桥（不启不停）。丢帧拿桥自己同期的 rx 增量当基准。"""
    m = measure(port, seconds, cpu_pid=None)
    link = req(port, "/api/link")
    rep = summarize(m["events"], m["dt"], fed_frames=m["fed"],
                    cpu=m["cpu"], log="attach:%d" % port)
    return {"rep": rep, "link": link}


# --------------------------------------------------- 定频回放（主机侧承载对照）
def make_replay(hz: int, frames: int) -> str:
    """拿真实录制里的 `src` 当模板，按 `hz` 重排 `t_host`，合成一份定频录制。"""
    src = None
    for path in sorted(glob.glob(os.path.join(CONSOLE, "data", "logs", "rec_*.jsonl"))):
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                if o.get("n") == "telem" and o.get("src"):
                    src = o["src"]
                    break
        if src:
            break
    if src is None:
        raise SystemExit("data/logs 里找不到 telem 录制行，没法当模板")
    base = 1_700_000_000_000
    step = 1000.0 / hz
    out = os.path.join(CONSOLE, "data", "logs", "bench_replay_%dhz.jsonl" % hz)
    with open(out, "w", encoding="utf-8") as fh:
        for i in range(frames):
            t = dict(src)
            t["t"] = int(i * step)
            fh.write(json.dumps({"n": "telem", "src": t, "t_host": base + int(i * step)},
                                ensure_ascii=False) + "\n")
    return out


def bench_replay(hz: int, seconds: float, low_res_timer: bool,
                 sse_batch_ms: int | None = None) -> dict:
    path = make_replay(hz, max(60, int(hz * (seconds + 2))))
    port = free_port()
    p, log = spawn(["--replay", path, "--replay-speed", "1.0"], port, "replay%dhz" % hz,
                   low_res_timer, sse_batch_ms)
    try:
        time.sleep(0.3)
        m = measure(port, seconds, cpu_pid=p.pid)
    finally:
        kill(p)
    rep = summarize(m["events"], m["dt"], fed_frames=m["fed"],
                    cpu=m["cpu"], log=log)
    rep["hz"] = hz
    return rep


# ------------------------------------------------------------ 串口模式（真车）
def bench_serial(dev: str, baud: int, seconds: float, low_res_timer: bool = False,
                 sse_batch_ms: int | None = None) -> dict:
    port = free_port()
    p, log = spawn(["--serial", dev, "--baud", str(baud)], port, "serial", low_res_timer,
                   sse_batch_ms)
    try:
        time.sleep(0.5)
        m = measure(port, seconds, cpu_pid=p.pid)
        link = req(port, "/api/link")
    finally:
        kill(p)
    rep = summarize(m["events"], m["dt"], fed_frames=m["fed"],
                    cpu=m["cpu"], log=log)
    rc, why = serial_verdict(link, rep["telem"])
    return {"rep": rep, "link": link, "rc": rc, "why": why}


# ------------------------------------------------------------- 仿真车：指令往返
def bench_sim(seconds: float, low_res_timer: bool = False,
              sse_batch_ms: int | None = None) -> dict:
    port = free_port()
    p, log = spawn(["--sim"], port, "sim", low_res_timer, sse_batch_ms)
    try:
        time.sleep(0.5)
        sse = Sse(port)
        sse.pump(1.0)
        req(port, "/api/cmd", {"cmd": "set_telem_hz", "arg": 100})
        time.sleep(0.5)
        rtts: list[float] = []
        drv: list[float] = []
        for _ in range(6):
            base = None
            for it, _t, _nb in expand(sse.pump(0.3)):
                if isinstance(it, dict) and "drive" in it:
                    base = it["drive"]
            if base is None:
                continue
            body = ({"mode": "wheels", "vl": 900, "vr": 900}
                    if base.get("mode", 0) != 1 else {"mode": "off"})
            t0 = time.monotonic()
            r = req(port, "/api/drive", body)
            rtts.append(r.get("rtt_ms"))
            seen = None
            while time.monotonic() < t0 + 3.0:
                for it, t, _nb in expand(sse.pump(0.2)):
                    if isinstance(it, dict) and it.get("drive") != base:
                        seen = (t - t0) * 1000.0
                        break
                if seen is not None:
                    break
            if seen is not None:
                drv.append(seen)
            time.sleep(0.4)
        req(port, "/api/drive", {"mode": "off"})
        link = req(port, "/api/link")
        sse.close()
    finally:
        kill(p)
    ok_rtt = [r for r in rtts if r]
    return {"rtt": ok_rtt, "drive_seen": drv, "link": link, "log": log,
            "rtt_p50": pct(ok_rtt, 50), "drive_p50": pct(drv, 50)}


def format_sim(sim: dict) -> list[str]:
    return [
        "  仿真车 100Hz：链路 ACK rtt_ms %s（p50 %.1f，n=%d）" % (
            ", ".join("%.0f" % r for r in sim["rtt"][:8]) or "无（仿真链路不记 ACK rtt）",
            sim["rtt_p50"], len(sim["rtt"])),
        "              发送指令 → 遥测里真的看见：%s（p50 %.1fms，n=%d）" % (
            ", ".join("%.0fms" % d for d in sim["drive_seen"]) or "没测到",
            sim["drive_p50"], len(sim["drive_seen"])),
        "              链路口径 %s | 日志 %s" % (
            json.dumps(sim["link"].get("stats") or {}, ensure_ascii=False),
            os.path.basename(sim["log"])),
    ]


def format_serial(b: dict) -> list[str]:
    link = b["link"]
    st = link.get("stats") or {}
    return [
        "  链路 %s @%s：connected=%s telem_hz=%s | rx=%s tx=%s crc=%s resync=%s drops=%s"
        % (link.get("port"), link.get("baud"), link.get("connected"), link.get("telem_hz"),
           st.get("rx"), st.get("tx"), st.get("crc"), st.get("resync"), st.get("drops")),
        "              固件回报 %s | 结论：%s" % (
            link.get("fw") or "（没收到固件版本）", b["why"]),
    ]


def main(argv=None) -> int:
    harden_stdio()          # 判定行里的 ⇒ 在 GBK 控制台上编不出来
    ap = argparse.ArgumentParser(prog="bench",
                                 description="主机侧承载量测（帧率/抖动/丢帧/CPU/延迟）")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--attach", type=int, metavar="PORT", help="量已经在跑的桥（不启不停）")
    g.add_argument("--serial", metavar="COM", help="自己起桥连串口（真车）")
    g.add_argument("--sim", action="store_true", help="自己起桥跑仿真车：量指令往返")
    g.add_argument("--replay", action="store_true", help="自己起桥跑定频回放：量主机侧承载")
    ap.add_argument("--baud", type=int, default=921600, help="--serial 用的波特率")
    ap.add_argument("--force-baud", action="store_true", dest="force_baud",
                    help="不拦 J-Link CDC 口上 >230400 的波特率（会让探针崩进 bootloader 态，慎用）")
    ap.add_argument("--hz", default="50,100,200", help="--replay 的定频档位，逗号分隔")
    ap.add_argument("--seconds", type=float, default=6.0, help="每档量多久，默认 6s")
    ap.add_argument("--low-res-timer", action="store_true",
                    help="起桥时不提高 Windows 计时器精度（对照：轮询掉到 ~64 次/s）")
    ap.add_argument("--sse-batch-ms", type=int, default=None, metavar="N",
                    help="起桥时用 N 毫秒攒批（0 = 不合并；不给就用桥的默认 20ms）")
    args = ap.parse_args(argv)
    batch = args.sse_batch_ms

    print("=" * 100)
    print("  主机侧承载量测  python %s  console=%s" % (sys.version.split()[0], CONSOLE))
    print("=" * 100)

    if args.attach:
        try:
            a = bench_attached(args.attach, args.seconds)
        except OSError as e:
            print("  连不上 %d 口上的桥（%s）⇒ 桥在跑吗？端口对不对？" % (args.attach, e))
            return 2
        for line in format_report(a["rep"], "已运行的桥 :%d" % args.attach):
            print(line)
        st = a["link"].get("stats") or {}
        print("              链路口径 %s | 板块 %s" % (
            json.dumps(st, ensure_ascii=False), a["link"].get("fw") or "（没回报固件版本）"))
        print("=" * 100)
        return 0

    if args.serial:
        err = check_baud(args.serial, args.baud, args.force_baud)
        if err:
            print("  拒绝：%s" % err)
            print("=" * 100)
            return 2
        b = bench_serial(args.serial, args.baud, args.seconds, args.low_res_timer, batch)
        for line in format_report(b["rep"], "%s @%d" % (args.serial, args.baud)):
            print(line)
        for line in format_serial(b):
            print(line)
        print("=" * 100)
        return b["rc"]

    if args.sim:
        sim = bench_sim(args.seconds, args.low_res_timer, batch)
        for line in format_sim(sim):
            print(line)
        print("=" * 100)
        return 0

    for hz in [int(x) for x in args.hz.split(",") if x.strip()]:
        rep = bench_replay(hz, args.seconds, args.low_res_timer, batch)
        for line in format_report(rep, "回放 %3dHz" % hz):
            print(line)
    print("=" * 100)
    return 0


if __name__ == "__main__":
    sys.exit(main())
