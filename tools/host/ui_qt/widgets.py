"""自绘控件（QPainter）：灰度条 / 航向表盘 / 速度条 / 状态格 / 参数总览。

用 QPainter 手绘而不是堆控件，是为了实时刷新时不闪、不吃 CPU。
"""

from __future__ import annotations

import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QBrush, QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import QGridLayout, QLabel, QSizePolicy, QWidget

from . import theme as T


def _c(s: str, a: int = 255) -> QColor:
    col = QColor(s)
    col.setAlpha(a)
    return col


class LineSensorBar(QWidget):
    """8 路灰度：压线的那几路变绿，下面标出加权偏差位置。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(74)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.mask = 0
        self.err = 0.0

    def update_telemetry(self, t) -> None:
        self.mask = t.line_mask
        self.err = t.line_error
        self.update()

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing, True)
        w, h = self.width(), self.height()
        pad = 8
        cell = (w - 2 * pad) / 8.0
        for i in range(8):
            on = (self.mask >> i) & 1
            r = QRectF(pad + i * cell + 2, 6, cell - 4, h - 30)
            p.setPen(QPen(_c(T.GREEN) if on else _c(T.BORDER), 1.2))
            p.setBrush(QBrush(_c(T.GREEN, 150) if on else _c("#1b212b")))
            p.drawRoundedRect(r, 4, 4)
            p.setFont(QFont("Consolas", 7))
            p.setPen(QPen(_c("#0b1a10") if on else _c(T.MUTED)))
            p.drawText(r, Qt.AlignCenter, str(i + 1))
        bar = QRectF(pad, h - 18, w - 2 * pad, 6)
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(_c("#1b212b")))
        p.drawRoundedRect(bar, 3, 3)
        x = pad + (self.err + 4.0) / 8.0 * (w - 2 * pad)
        p.setBrush(QBrush(_c(T.ACCENT)))
        p.drawEllipse(QPointF(x, h - 15), 4.5, 4.5)
        p.setFont(QFont("Consolas", 8))
        p.setPen(QPen(_c(T.MUTED)))
        p.drawText(w - 70, h - 5, "偏差 %+.2f" % self.err)


class HeadingDial(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumSize(132, 132)
        self.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Fixed)
        self.yaw = self.pitch = self.roll = 0.0

    def update_yaw(self, yaw, pitch=0.0, roll=0.0) -> None:
        self.yaw, self.pitch, self.roll = yaw, pitch, roll
        self.update()

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing, True)
        w, h = self.width(), self.height()
        c = QPointF(w / 2, h / 2)
        r = min(w, h) / 2 - 12
        p.setPen(QPen(_c(T.BORDER), 2))
        p.setBrush(QBrush(_c("#131a23")))
        p.drawEllipse(c, r, r)
        for ang in range(0, 360, 30):
            rad = math.radians(ang - 90)
            p.setPen(QPen(_c(T.MUTED) if ang % 90 else _c(T.TEXT), 1.6))
            p.drawLine(QPointF(c.x() + (r - 7) * math.cos(rad), c.y() + (r - 7) * math.sin(rad)),
                       QPointF(c.x() + r * math.cos(rad), c.y() + r * math.sin(rad)))
        p.setFont(QFont("Consolas", 7))
        for ang, lbl in ((0, "N"), (90, "E"), (180, "S"), (270, "W")):
            rad = math.radians(ang - 90)
            p.setPen(QPen(_c(T.MUTED)))
            p.drawText(QRectF(c.x() + (r - 17) * math.cos(rad) - 6,
                              c.y() + (r - 17) * math.sin(rad) - 7, 12, 14),
                       Qt.AlignCenter, lbl)
        rad = math.radians(self.yaw - 90)
        p.setPen(QPen(_c(T.ACCENT), 2.6, Qt.SolidLine, Qt.RoundCap))
        p.drawLine(c, QPointF(c.x() + (r - 12) * math.cos(rad),
                              c.y() + (r - 12) * math.sin(rad)))
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(_c(T.ACCENT)))
        p.drawEllipse(c, 3.4, 3.4)
        p.setFont(QFont("Consolas", 12, QFont.Bold))
        p.setPen(QPen(_c(T.TEXT)))
        p.drawText(QRectF(0, c.y() + r * 0.32, w, 18), Qt.AlignCenter, "%+.1f°" % self.yaw)
        p.setFont(QFont("Microsoft YaHei UI", 7))
        p.setPen(QPen(_c(T.MUTED)))
        p.drawText(QRectF(0, c.y() + r * 0.32 + 17, w, 14), Qt.AlignCenter,
                   "p %+.1f  r %+.1f" % (self.pitch, self.roll))


class SpeedBars(QWidget):
    def __init__(self, parent=None, vmax: float = 700.0):
        super().__init__(parent)
        self.setMinimumHeight(76)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.vmax = vmax
        self.v = (0.0, 0.0)
        self.pwm = (0, 0)

    def update_speeds(self, v_l, v_r, pwm_l, pwm_r) -> None:
        self.v = (v_l, v_r)
        self.pwm = (pwm_l, pwm_r)
        self.update()

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing, True)
        w = self.width()
        left = 44
        track_w = w - left - 12
        mid = left + track_w / 2
        for idx, (lbl, col, val) in enumerate((("左", T.GREEN, self.v[0]),
                                               ("右", "#ffa657", self.v[1]))):
            y = 8 + idx * 26
            p.setFont(QFont("Microsoft YaHei UI", 8))
            p.setPen(QPen(_c(T.MUTED)))
            p.drawText(QRectF(4, y, 36, 18), Qt.AlignLeft | Qt.AlignVCenter, lbl)
            p.setPen(Qt.NoPen)
            p.setBrush(QBrush(_c("#1b212b")))
            p.drawRoundedRect(QRectF(left, y + 3, track_w, 12), 6, 6)
            frac = max(-1.0, min(1.0, val / self.vmax))
            x = mid + frac * (track_w / 2)
            p.setBrush(QBrush(_c(col)))
            p.drawRoundedRect(QRectF(min(mid, x), y + 3, abs(x - mid), 12), 6, 6)
            p.setPen(QPen(_c(T.BORDER), 1))
            p.drawLine(QPointF(mid, y + 2), QPointF(mid, y + 16))
            p.setFont(QFont("Consolas", 8))
            p.setPen(QPen(_c(T.TEXT)))
            p.drawText(QRectF(left, y + 16, track_w, 12), Qt.AlignRight, "%+7.1f mm/s" % val)
        p.setFont(QFont("Consolas", 8))
        p.setPen(QPen(_c(T.MUTED)))
        p.drawText(QRectF(4, 60, w - 8, 14), Qt.AlignLeft,
                   "PWM  L %+5d    R %+5d" % self.pwm)


class StatGrid(QWidget):
    """键值状态格。"""

    def __init__(self, rows, parent=None, columns: int = 1):
        super().__init__(parent)
        self._v: dict[str, QLabel] = {}
        grid = QGridLayout(self)
        grid.setContentsMargins(2, 2, 2, 2)
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(3)
        for i, (key, label) in enumerate(rows):
            r, cols = divmod(i, columns)
            base = cols * 2
            k = QLabel(label)
            k.setProperty("role", "k")
            v = QLabel("-")
            v.setProperty("role", "v")
            v.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            grid.addWidget(k, r, base)
            grid.addWidget(v, r, base + 1)
            self._v[key] = v
        for c in range(columns):
            grid.setColumnStretch(c * 2 + 1, 1)

    def set(self, key: str, text: str, color: str = T.TEXT) -> None:
        lbl = self._v.get(key)
        if lbl is not None:
            lbl.setText(text)
            lbl.setStyleSheet("color: %s;" % color)


class ParamBars(QWidget):
    """参数总览：每个参数一条，画出**当前值在允许范围里的位置**。

    调参时一眼看出哪些已经贴着上限/下限（那些通常就是"调到头了"的信号）。
    绿=安全区，黄=接近边界，红=越界（正常不该出现）。
    """

    ROW = 22
    NAME_W = 132
    VALUE_W = 96

    def __init__(self, parent=None):
        super().__init__(parent)
        self.rows: list[tuple] = []          # (name, unit, value, vmin, vmax, group)
        self.changed: set[str] = set()       # 与默认值不同的参数名
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

    def set_params(self, rows) -> None:
        self.rows = [(s.name, s.unit, float(v), s.vmin, s.vmax, s.group) for s, v in rows]
        self.changed = {s.name for s, v in rows if abs(float(v) - s.default) > 1e-9}
        self.setMinimumHeight(len(self.rows) * self.ROW + 16)
        self.update()

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing, True)
        w = self.width()
        bar_x = self.NAME_W + 8
        bar_w = max(60, w - bar_x - self.VALUE_W - 8)
        f_name = QFont("Microsoft YaHei UI", 8)
        f_val = QFont("Consolas", 8)
        last_group = None
        y = 6
        for name, unit, val, vmin, vmax, group in self.rows:
            if group != last_group:
                last_group = group
                p.setFont(QFont("Microsoft YaHei UI", 7))
                p.setPen(QPen(_c(T.MUTED)))
                p.drawText(QRectF(4, y, w - 8, 12), Qt.AlignLeft, group)
                y += 14
            span = (vmax - vmin) or 1.0
            frac = max(0.0, min(1.0, (val - vmin) / span))
            p.setFont(f_name)
            # 与默认值不同 = 这次调过的，用黄色名字标出来
            tweaked = name in self.changed
            p.setPen(QPen(_c(T.YELLOW if tweaked else T.TEXT)))
            p.drawText(QRectF(8, y, self.NAME_W - 4, self.ROW),
                       Qt.AlignLeft | Qt.AlignVCenter,
                       ("* " + name) if tweaked else name)
            # 轨道（中点线 + 当前值）
            p.setPen(Qt.NoPen)
            p.setBrush(QBrush(_c("#1b212b")))
            p.drawRoundedRect(QRectF(bar_x, y + 6, bar_w, 9), 4, 4)
            edge = min(frac, 1.0 - frac)
            col = T.GREEN if edge > 0.15 else (T.YELLOW if edge > 0.05 else T.RED)
            p.setBrush(QBrush(_c(col)))
            x = bar_x + frac * bar_w
            p.drawRoundedRect(QRectF(x - 2.5, y + 3, 5, 15), 2, 2)
            p.setPen(QPen(_c(T.BORDER), 1))
            p.drawLine(QPointF(bar_x, y + 10.5), QPointF(bar_x + bar_w, y + 10.5))
            p.setFont(f_val)
            p.setPen(QPen(_c(T.MUTED)))
            p.drawText(QRectF(bar_x + bar_w + 4, y, self.VALUE_W - 8, self.ROW),
                       Qt.AlignRight | Qt.AlignVCenter,
                       ("%.4g %s" % (val, unit)).strip())
            y += self.ROW
