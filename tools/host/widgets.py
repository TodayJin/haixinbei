"""自绘控件：波形、灰度阵列、航向表盘、速度条、状态格。

全部基于 tk.Canvas，不依赖 matplotlib —— 少一个安装依赖，刷新也更快。
"""

from __future__ import annotations

import math
import tkinter as tk

BG = "#16171c"
PANEL = "#1e2027"
GRID = "#2c2f3a"
FG = "#d7dae0"
DIM = "#7b808f"
ACCENT = "#4fc3f7"

WAVE_CHANNELS = (
    ("yaw", "航向", "#4fc3f7", "°"),
    ("v_l", "左轮速", "#81c784", "mm/s"),
    ("v_r", "右轮速", "#ffb74d", "mm/s"),
    ("pwm_l", "左PWM", "#ba68c8", ""),
    ("pwm_r", "右PWM", "#f06292", ""),
    ("vbat", "电池", "#ffd54f", "V"),
    ("ibat", "电流", "#e57373", "A"),
    ("line_err", "循迹偏差", "#4db6ac", ""),
    ("loop_us", "帧耗时", "#90a4ae", "us"),
)


class WaveformView(tk.Canvas):
    """多通道分层波形：每个通道一条横向泳道，各自量程自适应。"""

    PAD_L = 64
    PAD_R = 62
    PAD_T = 8
    PAD_B = 20

    def __init__(self, master, **kw):
        kw.setdefault("bg", BG)
        kw.setdefault("highlightthickness", 0)
        super().__init__(master, **kw)
        self.window_s = 10.0
        self.visible: set[str] = {"yaw", "v_l", "v_r", "line_err"}
        self._last: tuple[list[float], dict] | None = None

    def set_window(self, seconds: float) -> None:
        self.window_s = max(1.0, float(seconds))

    def set_visible(self, keys) -> None:
        self.visible = set(keys)

    def update_data(self, times: list[float], data: dict[str, list[float]]) -> None:
        self._last = (times, data)
        self.redraw()

    def redraw(self) -> None:
        self.delete("all")
        # 隐藏标签页里 winfo_width() 返回 1，会误判成"还没布局"而不画任何东西，
        # 所以拿不到真实尺寸时退回配置尺寸。
        w, h = self.winfo_width(), self.winfo_height()
        if w <= 1:
            w = int(self["width"])
        if h <= 1:
            h = int(self["height"])
        if w < 40 or h < 40:
            return
        lanes = [c for c in WAVE_CHANNELS if c[0] in self.visible]
        if not lanes or not self._last:
            self.create_text(w / 2, h / 2, text="勾选上方通道后显示波形", fill=DIM)
            return
        times, data = self._last
        if not times:
            self.create_text(w / 2, h / 2, text="等待数据…", fill=DIM)
            return

        t_now = times[-1]
        t0 = t_now - self.window_s
        plot_w = w - self.PAD_L - self.PAD_R
        plot_h = h - self.PAD_T - self.PAD_B
        lane_h = plot_h / len(lanes)

        # 垂直网格（每秒一条）
        for k in range(int(self.window_s) + 1):
            x = self.PAD_L + plot_w * (1 - k / self.window_s)
            if x < self.PAD_L - 1:
                break
            self.create_line(x, self.PAD_T, x, h - self.PAD_B, fill=GRID)
            self.create_text(x, h - self.PAD_B + 9, text="-%ds" % k if k else "现在",
                             fill=DIM, font=("Microsoft YaHei UI", 7))

        # 只取时间窗内的样本
        i0 = 0
        for i in range(len(times) - 1, -1, -1):
            if times[i] < t0:
                i0 = i
                break

        for li, (key, label, color, unit) in enumerate(lanes):
            ys_all = data.get(key) or []
            if len(ys_all) != len(times):
                continue
            xs = times[i0:]
            ys = ys_all[i0:]
            if len(ys) < 2:
                continue
            y_lo, y_hi = min(ys), max(ys)
            if y_hi - y_lo < 1e-9:
                y_lo -= 0.5
                y_hi += 0.5
            pad = (y_hi - y_lo) * 0.12
            y_lo -= pad
            y_hi += pad
            top = self.PAD_T + li * lane_h
            bot = top + lane_h

            # 泳道分隔与标签
            if li:
                self.create_line(self.PAD_L, top, w - self.PAD_R, top, fill=GRID)
            self.create_text(self.PAD_L - 6, top + lane_h / 2, text=label, anchor="e",
                             fill=color, font=("Microsoft YaHei UI", 8))
            self.create_text(self.PAD_L - 6, top + 8, text="%.4g" % y_hi, anchor="e",
                             fill=DIM, font=("Consolas", 7))
            self.create_text(self.PAD_L - 6, bot - 8, text="%.4g" % y_lo, anchor="e",
                             fill=DIM, font=("Consolas", 7))

            # 抽稀：每像素列最多一个点
            step = max(1, len(xs) // max(1, int(plot_w)))
            pts = []
            for i in range(0, len(xs), step):
                px = self.PAD_L + plot_w * (1 - (t_now - xs[i]) / self.window_s)
                py = bot - (ys[i] - y_lo) / (y_hi - y_lo) * lane_h
                pts.extend((px, py))
            if len(pts) >= 4:
                self.create_line(*pts, fill=color, width=1.4)

            cur = ys[-1]
            self.create_text(w - self.PAD_R + 6, top + lane_h / 2,
                             text="%7.3g%s" % (cur, (" " + unit) if unit else ""),
                             anchor="w", fill=color, font=("Consolas", 8))


class LineSensorView(tk.Canvas):
    """8 路灰度阵列：哪几路压线一目了然，并画出加权后的线位置。"""

    def __init__(self, master, **kw):
        kw.setdefault("bg", PANEL)
        kw.setdefault("highlightthickness", 0)
        kw.setdefault("height", 108)
        super().__init__(master, **kw)
        self._items: list[int] = []
        self._last_mask = -1

    def _ensure(self) -> None:
        if self._items:
            return
        w = self.winfo_width() or int(self["width"])
        h = int(self["height"])
        cell_w = (w - 24) / 8.0
        for i in range(8):
            x0 = 12 + i * cell_w
            self._items.append(self.create_rectangle(
                x0 + 2, 22, x0 + cell_w - 2, 70, fill="#242731", outline="#3a3f4d"))
            self.create_text(x0 + cell_w / 2, 12, text=str(i + 1), fill=DIM,
                             font=("Consolas", 8))
        self.create_text(12, 84, text="加权偏差", anchor="w", fill=DIM,
                         font=("Microsoft YaHei UI", 8))
        self._err_text = self.create_text(72, 84, text="-", anchor="w", fill=ACCENT,
                                          font=("Consolas", 9, "bold"))
        self._marker = self.create_line(0, 74, 0, 96, fill=ACCENT, width=2)
        self._count = self.create_text(w - 12, 84, text="", anchor="e", fill=DIM,
                                       font=("Microsoft YaHei UI", 8))

    def update_telemetry(self, t) -> None:
        self._ensure()
        w = self.winfo_width() or int(self["width"])
        if t.line_mask != self._last_mask:
            self._last_mask = t.line_mask
            for i, item in enumerate(self._items):
                on = (t.line_mask >> i) & 1
                self.itemconfig(item, fill="#3ddc84" if on else "#242731",
                                outline="#3ddc84" if on else "#3a3f4d")
        e = t.line_error
        self.itemconfig(self._err_text, text="%+.2f" % e)
        cell_w = (w - 24) / 8.0
        x = 12 + (e + 4.0) / 8.0 * 8 * cell_w
        self.coords(self._marker, x, 72, x, 96)
        self.itemconfig(self._count, text="%d 路压线  掩码 0x%02X" % (t.line_count, t.line_mask))


class HeadingDial(tk.Canvas):
    """航向表盘。"""

    def __init__(self, master, size: int = 168, **kw):
        kw.setdefault("bg", PANEL)
        kw.setdefault("highlightthickness", 0)
        kw.setdefault("width", size)
        kw.setdefault("height", size)
        super().__init__(master, **kw)
        self.size = size
        self._built = False

    def _build(self) -> None:
        if self._built:
            return
        s = self.size
        c = s / 2
        r = c - 14
        self.create_oval(c - r, c - r, c + r, c + r, outline="#3a3f4d", width=2)
        for ang in range(0, 360, 30):
            rad = math.radians(ang - 90)
            x1, y1 = c + (r - 6) * math.cos(rad), c + (r - 6) * math.sin(rad)
            x2, y2 = c + r * math.cos(rad), c + r * math.sin(rad)
            self.create_line(x1, y1, x2, y2, fill="#3a3f4d")
        for ang, lbl in ((0, "N"), (90, "E"), (180, "S"), (270, "W")):
            rad = math.radians(ang - 90)
            self.create_text(c + (r + 8) * math.cos(rad), c + (r + 8) * math.sin(rad),
                             text=lbl, fill=DIM, font=("Consolas", 8))
        self._needle = self.create_line(c, c, c, c - r + 14, fill=ACCENT, width=3,
                                        arrow="last")
        self._hub = self.create_oval(c - 4, c - 4, c + 4, c + 4, fill=ACCENT,
                                     outline="")
        self._txt = self.create_text(c, c + r - 26, text="--", fill=FG,
                                     font=("Consolas", 12, "bold"))
        self._sub = self.create_text(c, c + r - 8, text="", fill=DIM,
                                     font=("Microsoft YaHei UI", 8))
        self._c = c
        self._r = r
        self._built = True

    def update_yaw(self, yaw: float, pitch: float = 0.0, roll: float = 0.0) -> None:
        self._build()
        rad = math.radians(yaw - 90)
        c, r = self._c, self._r
        self.coords(self._needle, c, c,
                    c + (r - 14) * math.cos(rad), c + (r - 14) * math.sin(rad))
        self.itemconfig(self._txt, text="%+.1f°" % yaw)
        self.itemconfig(self._sub, text="pitch %+.1f  roll %+.1f" % (pitch, roll))


class SpeedBars(tk.Canvas):
    """左右轮速度与 PWM 的双向条。"""

    def __init__(self, master, vmax: float = 700.0, **kw):
        kw.setdefault("bg", PANEL)
        kw.setdefault("highlightthickness", 0)
        kw.setdefault("height", 92)
        super().__init__(master, **kw)
        self.vmax = vmax
        self._built = False

    def _build(self) -> None:
        if self._built:
            return
        w = self.winfo_width() or int(self["width"])
        self.create_text(12, 14, text="左轮", anchor="w", fill="#81c784",
                         font=("Microsoft YaHei UI", 8))
        self.create_text(12, 50, text="右轮", anchor="w", fill="#ffb74d",
                         font=("Microsoft YaHei UI", 8))
        self.create_rectangle(48, 6, w - 12, 26, outline="#3a3f4d", fill="#20232b")
        self.create_rectangle(48, 42, w - 12, 62, outline="#3a3f4d", fill="#20232b")
        for x0, y0, y1 in ((48, 6, 26), (48, 42, 62)):
            mid = (x0 + w - 12) / 2
            self.create_line(mid, y0, mid, y1, fill="#4a4f5e")
        self._bar_l = self.create_rectangle(48, 7, 48, 25, fill="#81c784", outline="")
        self._bar_r = self.create_rectangle(48, 43, 48, 61, fill="#ffb74d", outline="")
        self._txt_l = self.create_text(w - 16, 16, text="", anchor="e", fill=FG,
                                       font=("Consolas", 9, "bold"))
        self._txt_r = self.create_text(w - 16, 52, text="", anchor="e", fill=FG,
                                       font=("Consolas", 9, "bold"))
        self._pwm = self.create_text(12, 78, text="", anchor="w", fill=DIM,
                                     font=("Consolas", 8))
        self._built = True

    def update_speeds(self, v_l: float, v_r: float, pwm_l: int, pwm_r: int) -> None:
        self._build()
        w = self.winfo_width() or int(self["width"])
        mid = (48 + w - 12) / 2
        half = (w - 12 - 48) / 2
        for bar, val, y0, y1 in ((self._bar_l, v_l, 7, 25), (self._bar_r, v_r, 43, 61)):
            frac = max(-1.0, min(1.0, val / self.vmax))
            x = mid + frac * half
            self.coords(bar, min(mid, x), y0, max(mid, x), y1)
        self.itemconfig(self._txt_l, text="%+7.1f mm/s" % v_l)
        self.itemconfig(self._txt_r, text="%+7.1f mm/s" % v_r)
        self.itemconfig(self._pwm, text="PWM  L %+6d   R %+6d" % (pwm_l, pwm_r))


class InfoGrid(tk.Frame):
    """键值状态格。"""

    def __init__(self, master, rows, **kw):
        kw.setdefault("bg", PANEL)
        super().__init__(master, **kw)
        self.values: dict[str, tk.Label] = {}
        for i, (key, label) in enumerate(rows):
            tk.Label(self, text=label, bg=PANEL, fg=DIM, anchor="w",
                     font=("Microsoft YaHei UI", 8)).grid(row=i, column=0, sticky="w",
                                                          padx=(10, 6), pady=1)
            v = tk.Label(self, text="-", bg=PANEL, fg=FG, anchor="e",
                         font=("Consolas", 9, "bold"))
            v.grid(row=i, column=1, sticky="e", padx=(0, 10), pady=1)
            self.values[key] = v
        self.grid_columnconfigure(1, weight=1)

    def set(self, key: str, text: str, color: str = FG) -> None:
        lbl = self.values.get(key)
        if lbl is not None:
            lbl.config(text=text, fg=color)

