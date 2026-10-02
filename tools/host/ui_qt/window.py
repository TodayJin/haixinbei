"""PySide6 主窗口 —— 一个**调参工具**。

只做三件事：看波形、看/改参数、把参数下发。串口/协议逻辑全在 device 里，
和界面无关，所以改界面不会碰到协议。
"""

from __future__ import annotations

import json
import os
import queue
import time

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QDialog,
                               QDialogButtonBox, QDoubleSpinBox, QFileDialog,
                               QFormLayout, QGroupBox, QHBoxLayout, QHeaderView,
                               QLabel, QListWidget, QMainWindow, QMessageBox,
                               QPlainTextEdit, QPushButton, QScrollArea,
                               QSizePolicy, QSplitter, QTableWidget,
                               QTableWidgetItem, QTabWidget, QToolBar, QVBoxLayout,
                               QWidget)

import metrics as M
import protocol as P
import recorder as R
import transport as Tr
from device import Device

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config.json")

from . import theme as T
from .widgets import HeadingDial, LineSensorBar, ParamBars, SpeedBars, StatGrid

WAVE_CHANNELS = (
    ("yaw", "航向", "#58a6ff", "°"),
    ("v_l", "左轮速", "#3fb950", "mm/s"),
    ("v_r", "右轮速", "#ffa657", "mm/s"),
    ("pwm_l", "左PWM", "#bc8cff", ""),
    ("pwm_r", "右PWM", "#f778ba", ""),
    ("vbat", "电池", "#e3b341", "V"),
    ("ibat", "电流", "#f85149", "A"),
    ("line_err", "循迹偏差", "#39c5cf", ""),
    ("loop_us", "帧耗时", "#8b949e", "us"),
)


class ParamDialog(QDialog):
    """改一个参数。"""

    def __init__(self, spec: P.ParamSpec, current: float, parent=None):
        super().__init__(parent)
        self.setWindowTitle("修改 %s" % spec.name)
        self.spec = spec
        form = QFormLayout(self)
        head = QLabel("%s  （%s）" % (spec.name, spec.group))
        head.setProperty("role", "h1")
        form.addRow(head)
        self.spin = QDoubleSpinBox()
        self.spin.setRange(spec.vmin, spec.vmax)
        self.spin.setDecimals(4)
        self.spin.setSingleStep(max(0.1, (spec.vmax - spec.vmin) / 200.0))
        self.spin.setValue(current)
        self.spin.setSuffix((" " + spec.unit) if spec.unit else "")
        self.spin.selectAll()
        form.addRow("数值", self.spin)
        form.addRow(QLabel("允许范围 %.6g ~ %.6g" % (spec.vmin, spec.vmax)))
        if spec.note:
            nb = QLabel("备注：%s" % spec.note)
            nb.setWordWrap(True)
            nb.setStyleSheet("color: %s;" % T.YELLOW)
            form.addRow(nb)
        btns = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        btns.button(QDialogButtonBox.Ok).setText("下发")
        btns.button(QDialogButtonBox.Cancel).setText("取消")
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)
        form.addRow(btns)

    def value(self) -> float:
        return self.spin.value()


class MainWindow(QMainWindow):
    def __init__(self, device: Device | None = None, load_config: bool = True):
        super().__init__()
        self.dev = device or Device()
        self._want_config = load_config
        self.q: queue.Queue = queue.Queue()
        self._last_telem = None
        self._t_wave = self._t_stat = self._t_bar = 0.0
        self._csv_path = ""
        self.baseline: dict[str, tuple[list, list]] = {}    # 调参基线（通道 -> (t, y)）
        self.base_curves: dict[str, object] = {}
        self.cursor_lines: dict[str, object] = {}
        self.cursor_data: tuple[list, dict] | None = None
        self._last_metrics = ""

        self.setWindowTitle("全地形机器人 · 上位机（调参台）")
        self.resize(1360, 860)

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
        if self._want_config:
            self._load_config()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)
        self.timer.start(33)

    # ------------------------------------------------------------ 工具条
    def _build_toolbar(self) -> None:
        tb = QToolBar()
        tb.setMovable(False)
        self.addToolBar(tb)

        tb.addWidget(QLabel("模式"))
        self.mode = QComboBox()
        self.mode.addItems(["演示数据", "串口", "回放"])
        self.mode.currentTextChanged.connect(self._on_mode)
        tb.addWidget(self.mode)

        tb.addWidget(QLabel("端口"))
        self.port = QComboBox()
        self.port.setMinimumWidth(230)
        tb.addWidget(self.port)
        b = QPushButton("刷新")
        b.clicked.connect(self.refresh_ports)
        tb.addWidget(b)

        tb.addWidget(QLabel("波特率"))
        self.baud = QComboBox()
        self.baud.addItems(["9600", "19200", "38400", "57600", "115200",
                            "230400", "460800", "921600"])
        self.baud.setCurrentText("115200")
        tb.addWidget(self.baud)

        tb.addWidget(QLabel("倍速"))
        self.speed = QComboBox()
        self.speed.addItems(["1", "2", "5", "10", "20"])
        tb.addWidget(self.speed)

        self.btn_conn = QPushButton("连接")
        self.btn_conn.setProperty("accent", True)
        self.btn_conn.clicked.connect(self.toggle_connect)
        tb.addWidget(self.btn_conn)

        spacer = QWidget()
        spacer.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        tb.addWidget(spacer)
        self.conn_lbl = QLabel("●  未连接")
        self.conn_lbl.setStyleSheet("color: %s; font-weight: 600;" % T.RED)
        tb.addWidget(self.conn_lbl)

    def _on_mode(self, text: str) -> None:
        self.port.setEnabled(text == "串口")

    def select_mode(self, text: str) -> bool:
        """按名字选模式；组合框里没有这一项时记日志并返回 False。

        （QComboBox.setCurrentText 对不存在的项是**静默无效**的，
        外面直接调会以为切成功了，实际还停在旧模式上 —— 踩过。）"""
        idx = self.mode.findText(text)
        if idx < 0:
            self.log_line("界面里没有『%s』这个模式，保持当前模式" % text)
            return False
        self.mode.setCurrentIndex(idx)
        return True

    # ------------------------------------------------------------ 主体
    def _build_body(self) -> None:
        split = QSplitter(Qt.Horizontal)
        split.addWidget(self._build_left())
        split.addWidget(self._build_tabs())
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        split.setSizes([380, 980])
        self.setCentralWidget(split)

    def _card(self, title: str, inner: QWidget) -> QGroupBox:
        box = QGroupBox(title)
        lay = QVBoxLayout(box)
        lay.setContentsMargins(10, 6, 10, 10)
        lay.addWidget(inner)
        return box

    def _build_left(self) -> QWidget:
        """左侧：实时信号的图形化显示（灰度条 / 表盘 / 轮速条 / 状态格）。"""
        left = QWidget()
        lay = QVBoxLayout(left)
        lay.setContentsMargins(10, 8, 6, 8)
        lay.setSpacing(10)

        self.sensors = LineSensorBar()
        lay.addWidget(self._card("循迹灰度（8 路）", self.sensors))

        row = QWidget()
        rl = QHBoxLayout(row)
        rl.setContentsMargins(0, 0, 0, 0)
        self.dial = HeadingDial()
        rl.addWidget(self.dial)
        self.info = StatGrid([("state", "步号"), ("vbat", "电池"), ("ibat", "电流"),
                              ("loop", "帧耗时"), ("rate", "帧率")])
        rl.addWidget(self.info, 1)
        lay.addWidget(self._card("姿态 / 状态", row))

        self.speeds = SpeedBars()
        lay.addWidget(self._card("轮速", self.speeds))

        self.flags_box = QListWidget()
        self.flags_box.setFont(QFont("Microsoft YaHei UI", 9))
        self.flags_box.setMaximumHeight(120)
        lay.addWidget(self._card("板上报的异常", self.flags_box), 1)
        return left

    def _build_tabs(self) -> QWidget:
        self.tabs = QTabWidget()
        self.tabs.addTab(self._tab_wave(), "波形")
        self.tabs.addTab(self._tab_param(), "参数")
        self.tabs.addTab(self._tab_param_view(), "参数总览")
        self.tabs.addTab(self._tab_cmd(), "命令")
        self.tabs.addTab(self._tab_record(), "记录")
        self.tabs.addTab(self._tab_log(), "日志")
        return self.tabs

    # ---- 波形 ----
    def _tab_wave(self) -> QWidget:
        """**每个通道一根独立量程的泳道** —— 共用一根 Y 轴的话，
        PWM(0~7200) 会把航向(±180)和电流压成一条直线，没法调参。"""
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(8, 8, 8, 8)
        top = QWidget()
        tl = QHBoxLayout(top)
        tl.setContentsMargins(0, 0, 0, 0)
        self.wave_vars = {}
        self.wave_curves = {}
        self.wave_lanes = {}
        pg.setConfigOptions(antialias=True)
        self.plotw = pg.GraphicsLayoutWidget()
        self.plotw.setBackground(T.PANEL)
        for pid, (key, label, color, unit) in enumerate(WAVE_CHANNELS):
            cb = QCheckBox(label)
            cb.setStyleSheet("color: %s;" % color)
            cb.setChecked(pid < 4)
            cb.stateChanged.connect(self._rebuild_lanes)
            self.wave_vars[key] = cb
            tl.addWidget(cb)
        tl.addStretch(1)
        b = QPushButton("存为基线")
        b.setToolTip("把当前窗口内的波形冻结成基线（虚线）。改完参数再跑，两条线一叠就知道差多少。")
        b.clicked.connect(self._store_baseline)
        tl.addWidget(b)
        b2 = QPushButton("清基线")
        b2.clicked.connect(self._clear_baseline)
        tl.addWidget(b2)
        tl.addWidget(QLabel("窗口"))
        self.win_sec = QComboBox()
        self.win_sec.addItems(["5", "10", "20", "30", "60"])
        self.win_sec.setCurrentText("10")
        tl.addWidget(self.win_sec)
        tl.addWidget(QLabel("秒"))
        lay.addWidget(top)
        lay.addWidget(self.plotw, 1)
        self.cursor_lbl = QLabel("把鼠标移到波形上可读数")
        self.cursor_lbl.setProperty("role", "muted")
        lay.addWidget(self.cursor_lbl)
        self.metrics_lbl = QLabel("")
        self.metrics_lbl.setFont(QFont("Consolas", 9))
        self.metrics_lbl.setProperty("role", "muted")
        self.metrics_lbl.setMinimumHeight(46)
        lay.addWidget(self.metrics_lbl)
        self.plotw.setMouseTracking(True)
        self.plotw.scene().sigMouseMoved.connect(self._on_plot_mouse)
        self._rebuild_lanes()
        return w

    def _rebuild_lanes(self) -> None:
        self.plotw.clear()
        self.wave_curves.clear()
        self.wave_lanes.clear()
        first = None
        row = 0
        for key, label, color, unit in WAVE_CHANNELS:
            if not self.wave_vars[key].isChecked():
                continue
            p = self.plotw.addPlot(row=row, col=0)
            p.showGrid(x=True, y=True, alpha=0.15)
            p.setLabel("left", label, units=unit)
            p.getAxis("left").setTextPen(pg.mkPen(QColor(color)))
            p.getAxis("left").setWidth(62)
            p.getAxis("bottom").setTextPen(pg.mkPen(T.MUTED))
            p.setDownsampling(auto=True, mode="peak")
            p.setClipToView(True)
            p.getViewBox().setBackgroundColor(None)
            for ax in ("top", "right"):
                p.showAxis(ax, False)
            if first is None:
                first = p
            else:
                p.setXLink(first)
            if row == 0:
                p.setLabel("bottom", "")
            self.wave_curves[key] = p.plot(pen=pg.mkPen(QColor(color), width=1.8))
            base_pen = pg.mkPen(QColor(color), width=1.3, style=Qt.DashLine)
            self.base_curves[key] = p.plot(pen=base_pen)
            line = pg.InfiniteLine(angle=90, movable=False,
                                   pen=pg.mkPen(QColor(T.MUTED), width=1,
                                                style=Qt.DotLine))
            line.setVisible(False)
            p.addItem(line, ignoreBounds=True)
            self.cursor_lines[key] = line
            if key in self.baseline:
                self._apply_baseline(key)
            self.wave_lanes[key] = p
            row += 1
        if row:
            self.plotw.ci.layout.setSpacing(2)
        keys = list(self.wave_lanes)
        for key in keys[:-1]:
            self.wave_lanes[key].getAxis("bottom").setStyle(showValues=False)

    # ---- 基线对比 / 读数 / 指标 ----
    def _store_baseline(self) -> None:
        """把当前窗口内的数据冻结成基线（虚线），用于"改参数前后对比"。"""
        times, data = self.dev.snapshot()
        if not times:
            self.log_line("还没数据，存不了基线")
            return
        t_now = times[-1]
        win = float(self.win_sec.currentText())
        i0 = 0
        for i in range(len(times) - 1, -1, -1):
            if times[i] < t_now - win:
                i0 = i
                break
        self.baseline = {k: ([x - t_now for x in times[i0:]], list((data.get(k) or [])[i0:]))
                         for k, _l, _c, _u in WAVE_CHANNELS if data.get(k)}
        for key in self.base_curves:
            self._apply_baseline(key)
        n = len(next(iter(self.baseline.values()))[0]) if self.baseline else 0
        self.log_line("已存基线：%d 点 / %.0f 秒（虚线）。改完参数再跑就能对比"
                      % (n, win))

    def _clear_baseline(self) -> None:
        self.baseline.clear()
        for key, curve in self.base_curves.items():
            curve.setData([], [])
        self.log_line("基线已清除")

    def _apply_baseline(self, key: str) -> None:
        curve = self.base_curves.get(key)
        pts = self.baseline.get(key)
        if curve is None or not pts:
            return
        curve.setData(pts[0], pts[1])

    def _on_plot_mouse(self, pos) -> None:
        """鼠标在波形上移动 -> 十字竖线 + 各通道该时刻的读数。"""
        if not self.wave_lanes:
            return
        first = list(self.wave_lanes.values())[0]
        vb = first.getViewBox()
        if not first.sceneBoundingRect().contains(pos):
            return
        x = vb.mapSceneToView(pos).x()          # 相对"现在"的秒数
        times, data = self.dev.snapshot()
        if not times:
            return
        t_now = times[-1]
        target = t_now + x
        idx = min(range(len(times)), key=lambda i: abs(times[i] - target))
        parts = []
        for key, label, _c, unit in WAVE_CHANNELS:
            ys = data.get(key) or []
            if not ys:
                continue
            line = self.cursor_lines.get(key)
            if line is not None:
                line.setVisible(True)
                line.setPos(x)
            if self.wave_vars[key].isChecked():
                parts.append("%s=%.4g%s" % (label, ys[idx], (" " + unit) if unit else ""))
        self.cursor_lbl.setText("t %+.2fs   %s" % (x, "   ".join(parts[:6])))

    def _update_metrics(self) -> None:
        """对当前窗口内的每个可见通道算量化指标 —— 调参别只靠眼睛。"""
        times, data = self.dev.snapshot()
        if not times:
            self.metrics_lbl.setText("")
            return
        t_now = times[-1]
        win = float(self.win_sec.currentText())
        i0 = 0
        for i in range(len(times) - 1, -1, -1):
            if times[i] < t_now - win:
                i0 = i
                break
        lines = []
        for key, label, _c, unit in WAVE_CHANNELS:
            if not self.wave_vars[key].isChecked():
                continue
            ys = (data.get(key) or [])[i0:]
            if len(ys) < 3:
                continue
            s = M.summarize(ys)
            lines.append("%-5s 峰峰 %-9.4g 均 %-9.4g 标准差 %-8.4g 过零 %d 次%s"
                         % (label, s["pp"], s["mean"], s["std"], s["zc"],
                            ("   [" + unit + "]") if unit else ""))
        self.metrics_lbl.setText("\n".join(lines))

    # ---- 参数 ----
    def _tab_param(self) -> QWidget:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(8, 8, 8, 8)
        top = QWidget()
        tl = QHBoxLayout(top)
        tl.setContentsMargins(0, 0, 0, 0)
        for text, fn in (("从车读取", self.dev.get_all_params),
                         ("全部下发", self._push_all),
                         ("写入 Flash", self.dev.save_params),
                         ("恢复默认", self._restore_defaults),
                         ("保存预设", self._save_preset),
                         ("载入预设", self._load_preset)):
            b = QPushButton(text)
            b.clicked.connect(fn)
            tl.addWidget(b)
        tl.addStretch(1)
        hint = QLabel("双击一行改值，改完立即下发")
        hint.setProperty("role", "muted")
        tl.addWidget(hint)
        lay.addWidget(top)

        self.table = QTableWidget(0, 6)
        self.table.setHorizontalHeaderLabels(["参数", "当前值", "单位", "允许范围",
                                              "分组", "备注"])
        self.table.verticalHeader().setVisible(False)
        self.table.setAlternatingRowColors(True)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.doubleClicked.connect(self._edit_param)
        lay.addWidget(self.table, 1)
        self._fill_params()
        return w

    def _tab_param_view(self) -> QWidget:
        """参数总览：每条画出"当前值在允许范围里的位置"，一眼看出谁调到头了。"""
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(8, 8, 8, 8)
        head = QLabel("每条 = 一个参数；标记位置 = 当前值在允许范围里的位置。"
                      "绿=有余量，黄=接近边界，红=越界。带 * 的是与默认值不同的项。")
        head.setProperty("role", "muted")
        head.setWordWrap(True)
        lay.addWidget(head)
        self.pbars = ParamBars()
        area = QScrollArea()
        area.setWidgetResizable(True)
        area.setWidget(self.pbars)
        lay.addWidget(area, 1)
        self.pbars.set_params(self.dev.param_snapshot())
        return w

    def _fill_params(self) -> None:
        rows = self.dev.param_snapshot()
        self.table.setRowCount(len(rows))
        for i, (spec, val) in enumerate(rows):
            for j, text in enumerate((spec.name, "%.6g" % val, spec.unit,
                                      "%.4g ~ %.4g" % (spec.vmin, spec.vmax),
                                      spec.group, spec.note)):
                item = QTableWidgetItem(text)
                if j != 5:
                    item.setTextAlignment(Qt.AlignCenter)
                if abs(float(val) - spec.default) > 1e-9:
                    item.setForeground(QColor(T.YELLOW))       # 这次调过的
                elif spec.note and j == 5:
                    item.setForeground(QColor(T.YELLOW))
                self.table.setItem(i, j, item)
        self.table.resizeColumnsToContents()
        self.table.horizontalHeader().setSectionResizeMode(5, QHeaderView.Stretch)
        if hasattr(self, "pbars"):
            self.pbars.set_params(rows)

    def _edit_param(self, index) -> None:
        spec = P.PARAMS[index.row()]
        cur = self.dev.params.get(spec.pid, spec.default)
        dlg = ParamDialog(spec, cur, self)
        if dlg.exec() != QDialog.Accepted:
            return
        ok, msg = self.dev.set_param(spec.key, dlg.value())
        if not ok:
            self.log_line("下发失败：%s" % msg)
        self._refresh_param_row(spec.pid)

    def _refresh_param_row(self, pid: int) -> None:
        spec = P.PARAM_BY_ID.get(pid)
        if spec is None:
            return
        for i in range(self.table.rowCount()):
            if self.table.item(i, 0).text() == spec.name:
                self.table.item(i, 1).setText("%.6g" % self.dev.params.get(pid, spec.default))
                break
        # ⚠ 这里必须**直接刷新**参数总览：早先只置了个标志位、没人消费它，
        # 结果改完参数条形图和"改过的项"都不动（实测发现）。
        if hasattr(self, "pbars"):
            self.pbars.set_params(self.dev.param_snapshot())

    def _push_all(self) -> None:
        if not self.dev.connected:
            self.log_line("未连接，无法下发")
            return
        self.log_line("下发全部参数：%d 条" % self.dev.push_all_params())

    def _restore_defaults(self) -> None:
        for spec in P.PARAMS:
            self.dev.params[spec.pid] = spec.default
        self._fill_params()
        self.log_line("已恢复默认值（未下发，点『全部下发』生效）")

    def _save_preset(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "保存参数预设",
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "presets",
                         "preset.json"), "JSON (*.json)")
        if path:
            self.dev.save_preset(path)
            self.log_line("预设已保存 -> %s" % path)

    def _load_preset(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "载入参数预设", "", "JSON (*.json)")
        if not path:
            return
        try:
            n, warn = self.dev.load_preset(path)
        except Exception as exc:
            QMessageBox.critical(self, "载入失败", str(exc))
            return
        self._fill_params()
        self.log_line("载入预设 %d 条%s" % (n, ("；" + "；".join(warn)) if warn else ""))

    # ---- 命令 ----
    def _tab_cmd(self) -> QWidget:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(10, 10, 10, 10)

        def group(title, items):
            box = QGroupBox(title)
            h = QHBoxLayout(box)
            for text, fn in items:
                b = QPushButton(text)
                b.setMinimumHeight(30)
                b.clicked.connect(fn)
                h.addWidget(b)
            h.addStretch(1)
            return box

        lay.addWidget(group("任务", [
            ("启动任务 A", lambda: self.dev.send_cmd(P.CMD_START_A)),
            ("启动任务 B", lambda: self.dev.send_cmd(P.CMD_START_B)),
            ("启动任务 C", lambda: self.dev.send_cmd(P.CMD_START_C)),
            ("停止", lambda: self.dev.send_cmd(P.CMD_STOP)),
        ]))
        lay.addWidget(group("维护", [
            ("陀螺零偏校准", lambda: self.dev.send_cmd(P.CMD_CALIB_GYRO)),
            ("清除故障", lambda: self.dev.send_cmd(P.CMD_CLEAR_FAULT)),
            ("软复位", self._confirm_reset),
        ]))
        box = QGroupBox("遥测与记录")
        h = QHBoxLayout(box)
        h.addWidget(QLabel("遥测频率"))
        self.hz = QComboBox()
        self.hz.addItems(["10", "20", "50", "100", "200"])
        self.hz.setCurrentText("50")
        h.addWidget(self.hz)
        b = QPushButton("下发")
        b.clicked.connect(lambda: self.dev.set_telem_hz(int(self.hz.currentText())))
        h.addWidget(b)
        b2 = QPushButton("开始车上记录")
        b2.clicked.connect(lambda: self.dev.send_cmd(P.CMD_SET_LOGGING, 1))
        h.addWidget(b2)
        b3 = QPushButton("停止车上记录")
        b3.clicked.connect(lambda: self.dev.send_cmd(P.CMD_SET_LOGGING, 0))
        h.addWidget(b3)
        h.addStretch(1)
        lay.addWidget(box)

        note = QLabel("比赛规则明确：作品应自主控制，不允许任何形式的远程控制干预。\n"
                      "所以本界面的命令页只能在运行间隙使用（调参、导出黑匣子），"
                      "绝不能用于比赛运行中的干预。")
        note.setWordWrap(True)
        note.setStyleSheet("color: %s;" % T.YELLOW)
        lay.addWidget(note)
        lay.addStretch(1)
        return w

    def _confirm_reset(self) -> None:
        if QMessageBox.question(self, "软复位", "确认复位主控吗？车会停下来。") == \
                QMessageBox.Yes:
            self.dev.send_cmd(P.CMD_RESET)

    # ---- 记录 ----
    def _tab_record(self) -> QWidget:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(10, 10, 10, 10)
        top = QWidget()
        tl = QHBoxLayout(top)
        tl.setContentsMargins(0, 0, 0, 0)
        self.btn_rec = QPushButton("开始录制 CSV")
        self.btn_rec.clicked.connect(self._toggle_record)
        tl.addWidget(self.btn_rec)
        b = QPushButton("导入 CSV 并回放")
        b.clicked.connect(self._open_replay)
        tl.addWidget(b)
        b2 = QPushButton("分析 CSV")
        b2.clicked.connect(self._analyze_csv)
        tl.addWidget(b2)
        b3 = QPushButton("打开输出目录")
        b3.clicked.connect(self._open_outdir)
        tl.addWidget(b3)
        tl.addStretch(1)
        lay.addWidget(top)
        self.rec_info = QPlainTextEdit()
        self.rec_info.setReadOnly(True)
        self.rec_info.setPlainText(
            "录制会把每一帧遥测原样写进 CSV（32 字节定长 + 主机时间戳）。\n"
            "回放可以把一段失败的过程反复重放，不用再去跑一次车。\n\n"
            "默认输出目录：out/host/")
        lay.addWidget(self.rec_info, 1)
        return w

    # ---- 日志 ----
    def _tab_log(self) -> QWidget:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(10, 10, 10, 10)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(1200)
        lay.addWidget(self.log, 1)
        b = QPushButton("清空")
        b.clicked.connect(self.log.clear)
        lay.addWidget(b)
        return w

    def log_line(self, msg: str) -> None:
        self.log.appendPlainText("[%s] %s" % (time.strftime("%H:%M:%S"), msg))

    # ------------------------------------------------------------ 会话记忆
    def _load_config(self) -> None:
        """记住上次用的模式/波特率/倍速/窗口/勾选通道，省得每次重设。"""
        try:
            with open(CONFIG_PATH, encoding="utf-8") as fh:
                cfg = json.load(fh)
        except Exception:
            return
        if cfg.get("baud"):
            self.baud.setCurrentText(str(cfg["baud"]))
        if cfg.get("speed"):
            self.speed.setCurrentText(str(cfg["speed"]))
        if cfg.get("window"):
            self.win_sec.setCurrentText(str(cfg["window"]))
        vis = cfg.get("visible")
        if vis:
            for key, cb in self.wave_vars.items():
                cb.blockSignals(True)
                cb.setChecked(key in vis)
                cb.blockSignals(False)
            self._rebuild_lanes()
        if cfg.get("mode"):
            self.select_mode(cfg["mode"])
        self.log_line("已恢复上次的设置（%s）" % cfg.get("saved", ""))

    def _save_config(self) -> None:
        cfg = {
            "saved": time.strftime("%Y-%m-%d %H:%M:%S"),
            "mode": self.mode.currentText(),
            "baud": self.baud.currentText(),
            "speed": self.speed.currentText(),
            "window": self.win_sec.currentText(),
            "visible": [k for k, cb in self.wave_vars.items() if cb.isChecked()],
        }
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
                json.dump(cfg, fh, ensure_ascii=False, indent=2)
        except Exception as exc:
            self.log_line("保存设置失败：%s" % exc)

    # ------------------------------------------------------------ 状态栏
    def _build_statusbar(self) -> None:
        self.status = QLabel("就绪")
        self.status.setProperty("role", "muted")
        self.statusBar().addWidget(self.status)

    # ------------------------------------------------------------ 串口
    def refresh_ports(self) -> None:
        self.port.clear()
        for dev, desc in Tr.list_serial_ports():
            self.port.addItem("%s — %s" % (dev, desc), dev)
        if self.port.count() == 0 and not Tr.HAVE_PYSERIAL:
            self.log_line("未安装 pyserial，真实串口不可用（演示数据不受影响）")

    def toggle_connect(self) -> None:
        if self.dev.connected:
            self.dev.close()
            self.btn_conn.setText("连接")
            self.conn_lbl.setText("●  未连接")
            self.conn_lbl.setStyleSheet("color: %s; font-weight: 600;" % T.RED)
            return
        # 连接流程**绝不弹模态框**：模式不对/没串口就记一行日志并回落到演示数据。
        # （以前这里弹 messagebox，用户点一次弹一次，体验很糟。）
        mode = self.mode.currentText()
        if mode == "回放":
            self.log_line("回放模式要先在『记录』页选 CSV；这次先连演示数据")
            self.select_mode("演示数据")
            mode = "演示数据"
        if mode == "串口" and self.port.count() == 0:
            self.log_line("没找到串口（点『刷新』试试）；这次先连演示数据")
            self.select_mode("演示数据")
            mode = "演示数据"
        try:
            if mode == "串口":
                self.dev.open_serial(self.port.currentData(),
                                     int(self.baud.currentText()))
            else:
                self.dev.open_sim(speed=float(self.speed.currentText()))
        except Exception as exc:
            self.log_line("连接失败：%s" % exc)
            self.btn_conn.setText("连接")
            self.conn_lbl.setText("●  未连接")
            self.conn_lbl.setStyleSheet("color: %s; font-weight: 600;" % T.RED)
            return
        self.btn_conn.setText("断开")
        self.conn_lbl.setText("●  已连接")
        self.conn_lbl.setStyleSheet("color: %s; font-weight: 600;" % T.GREEN)

    def _outdir(self) -> str:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(os.path.dirname(os.path.dirname(here)))
        d = os.path.join(root, "out", "host")
        os.makedirs(d, exist_ok=True)
        return d

    def _toggle_record(self) -> None:
        if self.dev.recording:
            n = self.dev.stop_recording()
            self.btn_rec.setText("开始录制 CSV")
            self.rec_info.appendPlainText("录制结束：%d 帧 -> %s" % (n, self._csv_path))
        else:
            path, _ = QFileDialog.getSaveFileName(
                self, "保存遥测 CSV",
                os.path.join(self._outdir(), time.strftime("host_%Y%m%d_%H%M%S.csv")),
                "CSV (*.csv)")
            if not path:
                return
            self._csv_path = path
            self.dev.start_recording(path)
            self.btn_rec.setText("停止录制")
            self.rec_info.appendPlainText("开始录制 -> %s" % path)

    def _open_replay(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "选择要回放的 CSV",
                                              self._outdir(), "CSV (*.csv)")
        if not path:
            return
        try:
            self.dev.open_replay(path, speed=float(self.speed.currentText()))
        except Exception as exc:
            QMessageBox.critical(self, "回放失败", str(exc))
            return
        self.mode.setCurrentText("回放")
        self.log_line("回放：%s" % path)

    def _analyze_csv(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "选择要分析的 CSV",
                                              self._outdir(), "CSV (*.csv)")
        if not path:
            return
        try:
            s = R.summarize(R.read_csv(path))
        except Exception as exc:
            QMessageBox.critical(self, "读取失败", str(exc))
            return
        self.rec_info.appendPlainText(
            "文件：%s\n帧数：%d\n时长：%.2f s\n平均帧率：%.1f Hz\n"
            "航向范围：%.1f° ~ %.1f°\n最大轮速：%.0f mm/s\n"
            "电池：%.2f ~ %.2f V\n最大电流：%.2f A\n"
            "疑似卡死帧：%d\n循迹丢线帧：%d\n黑标事件：%d 次\n出现过的步号：%s\n"
            % (os.path.basename(path), s.n, s.duration_s, s.rate_hz, s.yaw_min,
               s.yaw_max, s.v_max, s.vbat_min, s.vbat_max, s.ibat_max,
               s.stall_frames, s.lost_line_frames, s.marker_count, list(s.states)))

    def _open_outdir(self) -> None:
        os.startfile(self._outdir())          # noqa: S606

    # ------------------------------------------------------------ 刷新
    def _tick(self) -> None:
        drained = 0
        while drained < 400:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                break
            drained += 1
            kind = item[0]
            if kind == "telem":
                self._last_telem = item[1]
            elif kind == "status":
                self.log_line(item[1])
            elif kind == "text":
                self.log_line("板: %s" % item[1].rstrip())
            elif kind == "ack":
                if item[2] != P.ST_OK:
                    self.log_line("！%s 被拒绝：%s"
                                  % (item[1], P.STATUS_NAMES.get(item[2], item[2])))
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
            self.info.set("vbat", "%.2f V" % t.vbat,
                          T.RED if t.vbat_mv < 10500 else T.TEXT)
            self.info.set("ibat", "%.2f A" % t.ibat)
            self.info.set("loop", "%d us" % t.loop_us)
            self.info.set("rate", "%.0f Hz" % self.dev.state.telem_hz)
            flags = t.flag_list
            if self.flags_box.count() != len(flags) or \
                    [self.flags_box.item(i).text() for i in range(self.flags_box.count())] != flags:
                self.flags_box.clear()
                for f in flags:
                    self.flags_box.addItem("⚠ " + f)

        now = time.monotonic()
        if now - self._t_wave >= 0.1:
            self._t_wave = now
            times, data = self.dev.snapshot()
            if times:
                t_now = times[-1]
                win = float(self.win_sec.currentText())
                keep = max(2, int(len(times) * min(1.0, win / max(1e-6, times[-1] - times[0]))))
                xs = [x - t_now for x in times[-keep:]]
                for key, _label, _color, _unit in WAVE_CHANNELS:
                    curve = self.wave_curves.get(key)
                    if curve is None:
                        continue
                    ys = data.get(key) or []
                    if ys:
                        curve.setData(xs, ys[-len(xs):])
                if self.wave_lanes:
                    list(self.wave_lanes.values())[0].setXRange(-win, 0, padding=0)
                if now - self._t_bar >= 0.5:
                    self._t_bar = now
                    self._update_metrics()

        if now - self._t_stat >= 0.5:
            self._t_stat = now
            if self.dev.connected:
                bits = [self.dev.state.mode, self.dev.link_stats_text()]
                if self.dev.recording:
                    bits.append("● 录制中 %d 帧" % self.dev.recorder.count)
                if self.dev.hello is not None and self.dev.state.param_crc_ok is False:
                    bits.append("⚠ 参数表指纹不一致")
                self.status.setText("   |   ".join(bits))
            else:
                self.status.setText("未连接")

    def closeEvent(self, ev) -> None:
        try:
            self._save_config()
            self.timer.stop()
            if self.dev.recording:
                self.dev.stop_recording()
            self.dev.close()
        finally:
            super().closeEvent(ev)
