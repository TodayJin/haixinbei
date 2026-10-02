"""上位机主界面（tkinter）。

布局：
  ┌ 工具条：模式 / 串口 / 波特率 / 连接 / 倍速 ─────────────┐
  │ 左：灰度阵列 + 航向表盘 + 速度条 + 状态格              │
  │ 右：波形 | 参数 | 命令 | 记录 | 日志                    │
  └ 状态栏：链路统计 ────────────────────────────────────┘

线程模型：串口读线程只往 queue 里塞事件，界面在 after() 里取，
所以不存在跨线程操作 tk 的问题。
"""

from __future__ import annotations

import os
import queue
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import protocol as P
import recorder as R
import transport as T
from device import Device
from widgets import (ACCENT, BG, DIM, FG, GRID, PANEL, HeadingDial, InfoGrid,
                     LineSensorView, SpeedBars, WaveformView)

WAVE_HZ = 10.0          # 波形刷新率（界面刷新可以比遥测慢）


def _now_stamp() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S")


class HostApp(tk.Tk):
    def __init__(self, device: Device | None = None) -> None:
        super().__init__()
        self.title("全地形机器人上位机")
        self.geometry("1180x740")
        self.minsize(980, 620)
        self.configure(bg=BG)
        self.dev = device or Device()
        self.q: queue.Queue = queue.Queue()
        self._alive = True
        self._after_id: str | None = None
        self._t_last_wave = 0.0
        self._t_last_stat = 0.0
        self._last_telem = None
        self._log_lines = 0

        self._init_style()
        self._build_toolbar()
        self._build_body()
        self._build_statusbar()

        for ev, fn in (("telemetry", lambda t: self.q.put(("telem", t))),
                       ("status", lambda m: self.q.put(("status", m))),
                       ("text", lambda m: self.q.put(("text", m))),
                       ("ack", lambda d, s: self.q.put(("ack", d, s))),
                       ("param", lambda pid, v: self.q.put(("param", pid, v))),
                       ("hello", lambda h: self.q.put(("hello", h)))):
            self.dev.subscribe(ev, fn)

        self.refresh_ports()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self._after_id = self.after(50, self._tick)

    # ------------------------------------------------------------ 样式
    def _init_style(self) -> None:
        st = ttk.Style(self)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        st.configure(".", background=BG, foreground=FG, fieldbackground=PANEL)
        st.configure("TFrame", background=BG)
        st.configure("TLabel", background=BG, foreground=FG,
                     font=("Microsoft YaHei UI", 9))
        st.configure("TButton", background="#2a2e38", foreground=FG, borderwidth=0,
                     padding=(9, 4), font=("Microsoft YaHei UI", 9))
        st.map("TButton", background=[("active", "#39404f"), ("pressed", "#48505f")])
        st.configure("TNotebook", background=BG, borderwidth=0)
        st.configure("TNotebook.Tab", background="#22252d", foreground=DIM,
                     padding=(12, 6), font=("Microsoft YaHei UI", 9))
        st.map("TNotebook.Tab", background=[("selected", PANEL)],
               foreground=[("selected", ACCENT)])
        st.configure("Treeview", background=PANEL, fieldbackground=PANEL,
                     foreground=FG, rowheight=21, borderwidth=0,
                     font=("Microsoft YaHei UI", 9))
        st.configure("Treeview.Heading", background="#2a2e38", foreground=FG,
                     font=("Microsoft YaHei UI", 9))
        st.map("Treeview", background=[("selected", "#31435c")])
        st.configure("TCheckbutton", background=BG, foreground=FG,
                     font=("Microsoft YaHei UI", 9))
        st.map("TCheckbutton", background=[("active", BG)])
        st.configure("TLabelframe", background=BG, foreground=ACCENT)
        st.configure("TLabelframe.Label", background=BG, foreground=ACCENT,
                     font=("Microsoft YaHei UI", 9, "bold"))

    # ------------------------------------------------------------ 工具条
    def _build_toolbar(self) -> None:
        bar = tk.Frame(self, bg=PANEL, height=44)
        bar.pack(side="top", fill="x")
        bar.pack_propagate(False)
        pad = dict(side="left", padx=4, pady=8)

        tk.Label(bar, text="模式", bg=PANEL, fg=DIM,
                 font=("Microsoft YaHei UI", 9)).pack(**pad)
        self.mode = tk.StringVar(value="演示数据")
        for m in ("串口", "演示数据", "回放"):
            tk.Radiobutton(bar, text=m, value=m, variable=self.mode, bg=PANEL, fg=FG,
                           selectcolor=PANEL, activebackground=PANEL,
                           activeforeground=ACCENT, font=("Microsoft YaHei UI", 9),
                           command=self._on_mode).pack(**pad)

        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(bar, textvariable=self.port_var, width=30,
                                     state="readonly")
        self.port_box.pack(**pad)
        ttk.Button(bar, text="刷新", command=self.refresh_ports).pack(**pad)

        tk.Label(bar, text="波特率", bg=PANEL, fg=DIM,
                 font=("Microsoft YaHei UI", 9)).pack(**pad)
        self.baud_var = tk.StringVar(value="115200")
        ttk.Combobox(bar, textvariable=self.baud_var, width=8, state="readonly",
                     values=("9600", "19200", "38400", "57600", "115200", "230400",
                             "460800", "921600")).pack(**pad)

        self.btn_conn = ttk.Button(bar, text="连接", command=self.toggle_connect)
        self.btn_conn.pack(**pad)

        tk.Label(bar, text="倍速", bg=PANEL, fg=DIM,
                 font=("Microsoft YaHei UI", 9)).pack(**pad)
        self.speed_var = tk.StringVar(value="1")
        ttk.Combobox(bar, textvariable=self.speed_var, width=5, state="readonly",
                     values=("1", "2", "5", "10", "20")).pack(**pad)

        self.conn_dot = tk.Label(bar, text="●  未连接", bg=PANEL, fg="#e57373",
                                 font=("Microsoft YaHei UI", 9, "bold"))
        self.conn_dot.pack(side="right", padx=12)

    def _on_mode(self) -> None:
        self.port_box.configure(state="readonly" if self.mode.get() == "串口" else "disabled")

    # ------------------------------------------------------------ 主体
    def _build_body(self) -> None:
        body = tk.Frame(self, bg=BG)
        body.pack(side="top", fill="both", expand=True, padx=8, pady=(6, 0))

        left = tk.Frame(body, bg=BG, width=392)
        left.pack(side="left", fill="y", padx=(0, 8))
        left.pack_propagate(False)

        self._section(left, "循迹灰度（8 路 / I2C 模块）")
        self.sensors = LineSensorView(left, width=380)
        self.sensors.pack(fill="x", padx=2, pady=(0, 6))

        dial_row = tk.Frame(left, bg=BG)
        dial_row.pack(fill="x")
        self.dial = HeadingDial(dial_row, size=168)
        self.dial.pack(side="left")
        self.info = InfoGrid(dial_row, [
            ("state", "任务/步号"), ("flags", "状态标志"), ("vbat", "电池电压"),
            ("ibat", "母线电流"), ("loop", "主循环帧耗时"), ("rate", "遥测帧率"),
        ])
        self.info.pack(side="left", fill="both", expand=True)

        self.speeds = SpeedBars(left, vmax=700)
        self.speeds.pack(fill="x", pady=(6, 0))

        nb = ttk.Notebook(body)
        nb.pack(side="left", fill="both", expand=True)
        self._tab_wave(nb)
        self._tab_param(nb)
        self._tab_cmd(nb)
        self._tab_record(nb)
        self._tab_log(nb)

    def _section(self, parent, text: str) -> None:
        tk.Label(parent, text=text, bg=BG, fg=ACCENT, anchor="w",
                 font=("Microsoft YaHei UI", 9, "bold")).pack(fill="x", pady=(2, 2))

    # ---- 波形页 ----
    def _tab_wave(self, nb) -> None:
        f = tk.Frame(nb, bg=PANEL)
        nb.add(f, text="波形")
        top = tk.Frame(f, bg=PANEL)
        top.pack(fill="x", padx=8, pady=6)
        self.wave_vars: dict[str, tk.BooleanVar] = {}
        for key, label, color, _unit in self.wave_channels():
            var = tk.BooleanVar(value=key in ("yaw", "v_l", "v_r", "line_err"))
            self.wave_vars[key] = var
            tk.Checkbutton(top, text=label, variable=var, bg=PANEL, fg=color,
                           selectcolor=PANEL, activebackground=PANEL,
                           activeforeground=color, font=("Microsoft YaHei UI", 8),
                           command=self._apply_wave_visible).pack(side="left", padx=3)
        tk.Label(top, text="窗口", bg=PANEL, fg=DIM,
                 font=("Microsoft YaHei UI", 8)).pack(side="left", padx=(16, 2))
        self.win_var = tk.StringVar(value="10")
        for s in ("5", "10", "20", "30"):
            tk.Radiobutton(top, text=s + "s", value=s, variable=self.win_var,
                           bg=PANEL, fg=FG, selectcolor=PANEL, activebackground=PANEL,
                           font=("Microsoft YaHei UI", 8),
                           command=lambda: self.wave.set_window(float(self.win_var.get()))
                           ).pack(side="left")
        self.wave = WaveformView(f, width=740, height=560)
        self.wave.pack(fill="both", expand=True, padx=6, pady=(0, 6))
        self._apply_wave_visible()

    @staticmethod
    def wave_channels():
        from widgets import WAVE_CHANNELS
        return WAVE_CHANNELS

    def _apply_wave_visible(self) -> None:
        self.wave.set_visible([k for k, v in self.wave_vars.items() if v.get()])

    # ---- 参数页 ----
    def _tab_param(self, nb) -> None:
        f = tk.Frame(nb, bg=PANEL)
        nb.add(f, text="参数")
        top = tk.Frame(f, bg=PANEL)
        top.pack(fill="x", padx=8, pady=6)
        ttk.Button(top, text="从车读取", command=self.dev.get_all_params).pack(side="left", padx=2)
        ttk.Button(top, text="全部下发", command=self._push_all).pack(side="left", padx=2)
        ttk.Button(top, text="写入Flash", command=self.dev.save_params).pack(side="left", padx=2)
        ttk.Button(top, text="恢复默认", command=self._restore_defaults).pack(side="left", padx=2)
        ttk.Button(top, text="保存预设", command=self._save_preset).pack(side="left", padx=2)
        ttk.Button(top, text="载入预设", command=self._load_preset).pack(side="left", padx=2)
        tk.Label(top, text="双击一行改值；改完立即下发一台车", bg=PANEL, fg=DIM,
                 font=("Microsoft YaHei UI", 8)).pack(side="left", padx=10)

        cols = ("name", "value", "unit", "range", "group", "note")
        self.tree = ttk.Treeview(f, columns=cols, show="headings", height=20)
        heads = (("name", "参数", 150), ("value", "当前值", 100), ("unit", "单位", 60),
                 ("range", "允许范围", 150), ("group", "分组", 90), ("note", "备注", 260))
        for key, text, w in heads:
            self.tree.heading(key, text=text)
            self.tree.column(key, width=w, anchor="w" if key in ("name", "note") else "center")
        self.tree.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.tree.bind("<Double-1>", self._edit_param)
        self._fill_params()

    def _fill_params(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for spec, val in self.dev.param_snapshot():
            self.tree.insert("", "end", iid=str(spec.pid), values=(
                spec.name, "%.6g" % val, spec.unit,
                "%.4g ~ %.4g" % (spec.vmin, spec.vmax), spec.group, spec.note))

    def _edit_param(self, _evt=None) -> None:
        sel = self.tree.selection()
        if not sel:
            return
        pid = int(sel[0])
        spec = P.PARAM_BY_ID.get(pid)
        if spec is None:
            return
        cur = self.dev.params.get(pid, spec.default)
        dlg = _ParamDialog(self, spec, cur)
        self.wait_window(dlg)
        if dlg.result is None:
            return
        ok, msg = self.dev.set_param(spec.key, dlg.result)
        if not ok:
            self._log("下发失败：%s" % msg)
        self._refresh_param_row(pid)

    def _refresh_param_row(self, pid: int) -> None:
        if not self.tree.exists(str(pid)):
            return
        spec = P.PARAM_BY_ID[pid]
        val = self.dev.params.get(pid, spec.default)
        self.tree.item(str(pid), values=(
            spec.name, "%.6g" % val, spec.unit,
            "%.4g ~ %.4g" % (spec.vmin, spec.vmax), spec.group, spec.note))

    def _push_all(self) -> None:
        if not self.dev.connected:
            self._log("未连接，无法下发")
            return
        n = self.dev.push_all_params()
        self._log("下发全部参数：%d 条" % n)

    def _restore_defaults(self) -> None:
        for spec in P.PARAMS:
            self.dev.params[spec.pid] = spec.default
        self._fill_params()
        self._log("已恢复参数表默认值（未下发，点『全部下发』生效）")

    def _save_preset(self) -> None:
        path = filedialog.asksaveasfilename(
            title="保存参数预设", defaultextension=".json",
            initialdir=os.path.join(os.path.dirname(os.path.abspath(__file__)), "presets"),
            initialfile="preset.json", filetypes=[("JSON", "*.json")])
        if not path:
            return
        self.dev.save_preset(path)
        self._log("预设已保存 -> %s" % path)

    def _load_preset(self) -> None:
        path = filedialog.askopenfilename(
            title="载入参数预设",
            initialdir=os.path.join(os.path.dirname(os.path.abspath(__file__)), "presets"),
            filetypes=[("JSON", "*.json")])
        if not path:
            return
        try:
            n, warn = self.dev.load_preset(path)
        except Exception as exc:
            messagebox.showerror("载入失败", str(exc))
            return
        self._fill_params()
        self._log("载入预设 %d 条%s" % (n, ("；" + "；".join(warn)) if warn else ""))

    # ---- 命令页 ----
    def _tab_cmd(self, nb) -> None:
        f = tk.Frame(nb, bg=PANEL)
        nb.add(f, text="命令")
        box = tk.LabelFrame(f, text="任务", bg=PANEL, fg=ACCENT)
        box.pack(fill="x", padx=10, pady=8)
        for text, cmd in (("启动任务A", P.CMD_START_A), ("启动任务B", P.CMD_START_B),
                          ("启动任务C", P.CMD_START_C), ("停止", P.CMD_STOP)):
            ttk.Button(box, text=text, width=14,
                       command=lambda c=cmd: self.dev.send_cmd(c)).pack(side="left", padx=6, pady=8)

        box2 = tk.LabelFrame(f, text="维护", bg=PANEL, fg=ACCENT)
        box2.pack(fill="x", padx=10, pady=8)
        for text, cmd in (("陀螺零偏校准", P.CMD_CALIB_GYRO),
                          ("清除故障", P.CMD_CLEAR_FAULT)):
            ttk.Button(box2, text=text, width=14,
                       command=lambda c=cmd: self.dev.send_cmd(c)).pack(side="left", padx=6, pady=8)
        ttk.Button(box2, text="软复位", width=14,
                   command=self._confirm_reset).pack(side="left", padx=6, pady=8)

        box3 = tk.LabelFrame(f, text="遥测与记录", bg=PANEL, fg=ACCENT)
        box3.pack(fill="x", padx=10, pady=8)
        tk.Label(box3, text="遥测频率(Hz)", bg=PANEL, fg=FG,
                 font=("Microsoft YaHei UI", 9)).pack(side="left", padx=(10, 4), pady=8)
        self.hz_var = tk.StringVar(value="50")
        ttk.Combobox(box3, textvariable=self.hz_var, width=6, state="readonly",
                     values=("10", "20", "50", "100", "200")).pack(side="left")
        ttk.Button(box3, text="下发", width=8,
                   command=lambda: self.dev.set_telem_hz(int(self.hz_var.get()))
                   ).pack(side="left", padx=8)
        ttk.Button(box3, text="开始车上记录", width=14,
                   command=lambda: self.dev.send_cmd(P.CMD_SET_LOGGING, 1)
                   ).pack(side="left", padx=6)
        ttk.Button(box3, text="停止车上记录", width=14,
                   command=lambda: self.dev.send_cmd(P.CMD_SET_LOGGING, 0)
                   ).pack(side="left", padx=6)

        tk.Label(f, text="说明：命令与参数下发都走 LPUART1（无线下载器虚拟串口）；"
                         "车上记录指 MCU 侧黑匣子，和本机 CSV 录制是两件事。",
                 bg=PANEL, fg=DIM, justify="left",
                 font=("Microsoft YaHei UI", 8)).pack(fill="x", padx=14, pady=6)

    def _confirm_reset(self) -> None:
        if messagebox.askyesno("软复位", "确认要复位主控吗？车会停下来。"):
            self.dev.send_cmd(P.CMD_RESET)

    # ---- 记录页 ----
    def _tab_record(self, nb) -> None:
        f = tk.Frame(nb, bg=PANEL)
        nb.add(f, text="记录/回放")
        top = tk.Frame(f, bg=PANEL)
        top.pack(fill="x", padx=10, pady=8)
        self.btn_rec = ttk.Button(top, text="开始录制 CSV", command=self._toggle_record)
        self.btn_rec.pack(side="left")
        ttk.Button(top, text="导入 CSV 并回放", command=self._open_replay).pack(side="left", padx=8)
        ttk.Button(top, text="分析 CSV", command=self._analyze_csv).pack(side="left", padx=8)
        ttk.Button(top, text="打开输出目录", command=self._open_outdir).pack(side="left", padx=8)

        self.rec_info = tk.Text(f, bg=BG, fg=FG, insertbackground=FG, height=24,
                                relief="flat", font=("Consolas", 9), wrap="word")
        self.rec_info.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        intro = (
            "录制会把每一帧遥测原样写进 CSV（32 字节定长 + 主机时间戳）。",
            "回放可以把一段失败的过程反复重放，不用再去跑一次车。",
            "",
            "默认输出目录：out/host/",
        )
        self.rec_info.insert("1.0", "\n".join(intro) + "\n")
        self.rec_info.configure(state="disabled")

    def _outdir(self) -> str:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(os.path.dirname(here))
        d = os.path.join(root, "out", "host")
        os.makedirs(d, exist_ok=True)
        return d

    def _toggle_record(self) -> None:
        if self.dev.recording:
            n = self.dev.stop_recording()
            self.btn_rec.config(text="开始录制 CSV")
            self._rec_write("录制结束：%d 帧 -> %s\n" % (n, self._current_csv))
        else:
            default = time.strftime("host_%Y%m%d_%H%M%S.csv")
            path = filedialog.asksaveasfilename(
                title="保存遥测 CSV", defaultextension=".csv",
                initialdir=self._outdir(), initialfile=default,
                filetypes=[("CSV", "*.csv")])
            if not path:
                return
            self._current_csv = path
            self.dev.start_recording(path)
            self.btn_rec.config(text="停止录制")
            self._rec_write("开始录制 -> %s\n" % path)

    def _open_replay(self) -> None:
        path = filedialog.askopenfilename(title="选择要回放的 CSV",
                                          initialdir=self._outdir(),
                                          filetypes=[("CSV", "*.csv")])
        if not path:
            return
        try:
            self.dev.open_replay(path, speed=float(self.speed_var.get()))
        except Exception as exc:
            messagebox.showerror("回放失败", str(exc))
            return
        self.mode.set("回放")
        self._log("回放：%s" % path)

    def _analyze_csv(self) -> None:
        path = filedialog.askopenfilename(title="选择要分析的 CSV",
                                          initialdir=self._outdir(),
                                          filetypes=[("CSV", "*.csv")])
        if not path:
            return
        try:
            frames = R.read_csv(path)
        except Exception as exc:
            messagebox.showerror("读取失败", str(exc))
            return
        s = R.summarize(frames)
        txt = ("文件：%s\n帧数：%d\n时长：%.2f s\n平均帧率：%.1f Hz\n"
               "航向范围：%.1f° ~ %.1f°\n最大轮速：%.0f mm/s\n"
               "电池：%.2f ~ %.2f V\n最大电流：%.2f A\n"
               "疑似卡死帧：%d\n循迹丢线帧：%d\n黑标事件：%d 次\n出现过的步号：%s\n\n"
               % (os.path.basename(path), s.n, s.duration_s, s.rate_hz,
                  s.yaw_min, s.yaw_max, s.v_max, s.vbat_min, s.vbat_max,
                  s.ibat_max, s.stall_frames, s.lost_line_frames, s.marker_count,
                  list(s.states)))
        self._rec_write(txt)

    def _rec_write(self, text: str) -> None:
        self.rec_info.configure(state="normal")
        self.rec_info.insert("end", text)
        self.rec_info.see("end")
        self.rec_info.configure(state="disabled")

    def _open_outdir(self) -> None:
        os.startfile(self._outdir())          # noqa: S606 (Windows 专用)

    # ---- 日志页 ----
    def _tab_log(self, nb) -> None:
        f = tk.Frame(nb, bg=PANEL)
        nb.add(f, text="日志")
        self.log = tk.Text(f, bg=BG, fg=FG, insertbackground=FG, relief="flat",
                           font=("Consolas", 9), wrap="none")
        self.log.pack(fill="both", expand=True, padx=8, pady=8)
        self.log.configure(state="disabled")
        ttk.Button(f, text="清空", command=self._clear_log).pack(pady=(0, 8))

    def _clear_log(self) -> None:
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self._log_lines = 0

    def _log(self, msg: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", "[%s] %s\n" % (time.strftime("%H:%M:%S"), msg))
        self._log_lines += 1
        if self._log_lines > 800:
            self.log.delete("1.0", "200.0")
            self._log_lines -= 200
        self.log.see("end")
        self.log.configure(state="disabled")

    # ------------------------------------------------------------ 状态栏
    def _build_statusbar(self) -> None:
        bar = tk.Frame(self, bg=PANEL, height=26)
        bar.pack(side="bottom", fill="x")
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text="就绪", bg=PANEL, fg=DIM, anchor="w",
                               font=("Consolas", 8))
        self.status.pack(side="left", padx=10)

    # ------------------------------------------------------------ 串口
    def refresh_ports(self) -> None:
        ports = T.list_serial_ports()
        if not ports:
            self.port_box.configure(values=[])
            self.port_var.set("")
            if not T.HAVE_PYSERIAL:
                self._log("未安装 pyserial，真实串口不可用（模拟器不受影响）")
            return
        vals = ["%s — %s" % (dev, desc) for dev, desc in ports]
        self.port_box.configure(values=vals)
        if not self.port_var.get() or self.port_var.get() not in vals:
            self.port_var.set(vals[0])

    def toggle_connect(self) -> None:
        if self.dev.connected:
            self.dev.close()
            self.btn_conn.config(text="连接")
            self.conn_dot.config(text="●  未连接", fg="#e57373")
            return
        # 连接流程不弹模态框：模式不对/没串口就记日志并回落到演示数据
        mode = self.mode.get()
        if mode == "回放":
            self._log("回放模式要先在『记录』页选 CSV；这次先连演示数据")
            self.mode.set("演示数据")
            mode = "演示数据"
        try:
            if mode == "串口":
                raw = self.port_var.get()
                if not raw:
                    self._log("没找到串口；这次先连演示数据")
                    self.mode.set("演示数据")
                    self.dev.open_sim(speed=float(self.speed_var.get()))
                else:
                    port = raw.split(" — ")[0].strip()
                    self.dev.open_serial(port, int(self.baud_var.get()))
            else:
                self.dev.open_sim(speed=float(self.speed_var.get()))
        except Exception as exc:
            self._log("连接失败：%s" % exc)
            self.btn_conn.config(text="连接")
            self.conn_dot.config(text="●  未连接", fg="#e57373")
            return
        self.btn_conn.config(text="断开")
        self.conn_dot.config(text="●  已连接", fg="#3ddc84")

    # ------------------------------------------------------------ 主循环
    def _tick(self) -> None:
        if not self._alive:
            return
        drained = 0
        while drained < 300:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                break
            drained += 1
            kind = item[0]
            if kind == "telem":
                self._last_telem = item[1]
            elif kind == "status":
                self._log(item[1])
            elif kind == "text":
                self._log("板: %s" % item[1].rstrip())
            elif kind == "ack":
                desc, st = item[1], item[2]
                if st != P.ST_OK:
                    self._log("！%s 被拒绝：%s" % (desc, P.STATUS_NAMES.get(st, st)))
            elif kind == "param":
                self._refresh_param_row(item[1])
            elif kind == "hello":
                self._fill_params()

        t = self._last_telem
        if t is not None:
            self.sensors.update_telemetry(t)
            self.dial.update_yaw(t.yaw, t.pitch, t.roll)
            self.speeds.update_speeds(t.v_l_mmps, t.v_r_mmps, t.pwm_l, t.pwm_r)
            self.info.set("state", "%d" % t.state_id)
            flags = "、".join(t.flag_list) or "—"
            self.info.set("flags", flags,
                          "#e57373" if t.flags & (P.F_STUCK | P.F_FAULT) else FG)
            self.info.set("vbat", "%.2f V" % t.vbat,
                          "#e57373" if t.vbat_mv < 10500 else FG)
            self.info.set("ibat", "%.2f A" % t.ibat)
            self.info.set("loop", "%d us" % t.loop_us)

        now = time.monotonic()
        if now - self._t_last_wave >= 1.0 / WAVE_HZ:
            self._t_last_wave = now
            times, data = self.dev.snapshot()
            self.wave.update_data(times, data)
        if now - self._t_last_stat >= 0.5:
            self._t_last_stat = now
            self._refresh_status(t)
        if self._alive:
            self._after_id = self.after(40, self._tick)

    def _refresh_status(self, t) -> None:
        parts = []
        if self.dev.connected:
            parts.append("模式 %s" % self.dev.state.mode)
            parts.append(self.dev.link_stats_text())
            if t is not None:
                self.info.set("rate", "%.0f Hz" % self.dev.state.telem_hz)
        else:
            parts.append("未连接")
            self.info.set("rate", "-")
        if self.dev.recording:
            parts.append("● 录制中 %d 帧" % self.dev.recorder.count)
        if self.dev.hello is not None and self.dev.state.param_crc_ok is False:
            parts.append("⚠ 参数表指纹不一致")
        self.status.config(text="   |   ".join(parts))

    def _on_close(self) -> None:
        self._alive = False
        if self._after_id is not None:
            try:
                self.after_cancel(self._after_id)
            except tk.TclError:
                pass
            self._after_id = None
        try:
            if self.dev.recording:
                self.dev.stop_recording()
            self.dev.close()
        finally:
            self.destroy()


class _ParamDialog(tk.Toplevel):
    """改一个参数的小对话框。"""

    def __init__(self, master, spec: P.ParamSpec, current: float):
        super().__init__(master)
        self.title("修改 %s" % spec.name)
        self.configure(bg=BG)
        self.resizable(False, False)
        self.result: float | None = None
        self.spec = spec

        tk.Label(self, text=spec.name, bg=BG, fg=ACCENT,
                 font=("Microsoft YaHei UI", 10, "bold")).grid(row=0, column=0,
                                                               columnspan=2, pady=(10, 2))
        tk.Label(self, text="范围 %.6g ~ %.6g %s" % (spec.vmin, spec.vmax, spec.unit),
                 bg=BG, fg=DIM, font=("Microsoft YaHei UI", 8)).grid(row=1, column=0,
                                                                     columnspan=2)
        if spec.note:
            tk.Label(self, text=spec.note, bg=BG, fg="#ffd54f", wraplength=280,
                     justify="left", font=("Microsoft YaHei UI", 8)).grid(
                row=2, column=0, columnspan=2, padx=10, pady=(2, 6))

        self.var = tk.StringVar(value="%.6g" % current)
        ent = ttk.Entry(self, textvariable=self.var, width=18, font=("Consolas", 10))
        ent.grid(row=3, column=0, padx=(12, 4), pady=6)
        tk.Label(self, text=spec.unit, bg=BG, fg=DIM).grid(row=3, column=1, sticky="w")
        ent.focus_set()
        ent.select_range(0, "end")
        ent.bind("<Return>", lambda e: self._ok())

        row = tk.Frame(self, bg=BG)
        row.grid(row=4, column=0, columnspan=2, pady=(2, 12))
        ttk.Button(row, text="下发", command=self._ok).pack(side="left", padx=6)
        ttk.Button(row, text="取消", command=self.destroy).pack(side="left", padx=6)
        self.transient(master)
        self.grab_set()

    def _ok(self) -> None:
        try:
            val = float(self.var.get())
        except ValueError:
            messagebox.showerror("输入无效", "请输入数字", parent=self)
            return
        self.result = val
        self.destroy()
