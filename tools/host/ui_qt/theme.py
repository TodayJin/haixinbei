"""Qt 深色主题（QSS）。配色走 GitHub-dark 一类的低饱和深色，避免刺眼。"""

BG = "#0d1117"
PANEL = "#161b22"
PANEL2 = "#1c2230"
BORDER = "#2a3140"
TEXT = "#c9d1d9"
MUTED = "#8b949e"
ACCENT = "#58a6ff"
GREEN = "#3fb950"
YELLOW = "#d29922"
RED = "#f85149"
PURPLE = "#bc8cff"

QSS = """
QWidget { background: %(BG)s; color: %(TEXT)s;
          font-family: "Microsoft YaHei UI", "Segoe UI"; font-size: 12px; }
QMainWindow, QDialog { background: %(BG)s; }

QToolBar { background: %(PANEL)s; border: none; border-bottom: 1px solid %(BORDER)s;
           padding: 6px 8px; spacing: 8px; }
QToolBar QLabel { color: %(MUTED)s; }
QToolBar::separator { background: %(BORDER)s; width: 1px; margin: 4px 6px; }

QStatusBar { background: %(PANEL)s; color: %(MUTED)s;
             border-top: 1px solid %(BORDER)s; }
QStatusBar::item { border: none; }

QPushButton { background: %(PANEL2)s; color: %(TEXT)s; border: 1px solid %(BORDER)s;
              border-radius: 6px; padding: 5px 12px; }
QPushButton:hover { background: #232b3a; border-color: %(ACCENT)s; }
QPushButton:pressed { background: #2b3547; }
QPushButton:disabled { color: #4a5260; border-color: #222836; }
QPushButton[accent="true"] { background: %(ACCENT)s; color: #08111f; border: none;
                             font-weight: 600; }
QPushButton[accent="true"]:hover { background: #79b8ff; }
QPushButton[danger="true"] { background: %(RED)s; color: #fff; border: none; }

QComboBox { background: %(PANEL2)s; border: 1px solid %(BORDER)s; border-radius: 6px;
            padding: 4px 8px; min-width: 90px; }
QComboBox:hover { border-color: %(ACCENT)s; }
QComboBox QAbstractItemView { background: %(PANEL2)s; border: 1px solid %(BORDER)s;
                              selection-background-color: %(ACCENT)s;
                              selection-color: #08111f; }

QTabWidget::pane { border: 1px solid %(BORDER)s; border-radius: 8px; background: %(PANEL)s; }
QTabBar::tab { background: transparent; color: %(MUTED)s; padding: 7px 16px;
               margin-right: 2px; border-top-left-radius: 6px; border-top-right-radius: 6px; }
QTabBar::tab:hover { color: %(TEXT)s; }
QTabBar::tab:selected { background: %(PANEL)s; color: %(ACCENT)s;
                        border: 1px solid %(BORDER)s; border-bottom-color: %(PANEL)s; }

QGroupBox { border: 1px solid %(BORDER)s; border-radius: 8px; margin-top: 14px;
            background: %(PANEL)s; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px;
                   color: %(ACCENT)s; }

QTableWidget { background: %(PANEL)s; alternate-background-color: %(PANEL2)s;
               gridline-color: %(BORDER)s; border: none; }
QTableWidget::item { padding: 4px 6px; }
QTableWidget::item:selected { background: #1f3b5c; color: %(TEXT)s; }
QHeaderView::section { background: %(PANEL2)s; color: %(MUTED)s; border: none;
                       border-right: 1px solid %(BORDER)s;
                       border-bottom: 1px solid %(BORDER)s; padding: 6px; }

QPlainTextEdit, QTextEdit { background: #0a0e14; border: 1px solid %(BORDER)s;
                            border-radius: 6px; color: %(TEXT)s;
                            font-family: Consolas, monospace; font-size: 11px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #2f3947; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #3d4a5c; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #2f3947; border-radius: 5px; min-width: 24px; }

QLabel[role="h1"] { font-size: 15px; font-weight: 600; color: %(TEXT)s; }
QLabel[role="muted"] { color: %(MUTED)s; }
QLabel[role="big"] { font-size: 22px; font-weight: 700; }
QLabel[role="k"] { color: %(MUTED)s; }
QLabel[role="v"] { color: %(TEXT)s; font-weight: 600; }

QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: 4px;
                       border: 1px solid %(BORDER)s; background: %(PANEL2)s; }
QCheckBox::indicator:checked { background: %(ACCENT)s; border-color: %(ACCENT)s; }

QSplitter::handle { background: %(BORDER)s; }
QSplitter::handle:horizontal { width: 2px; }
QSlider::groove:horizontal { height: 4px; background: %(BORDER)s; border-radius: 2px; }
QSlider::handle:horizontal { background: %(ACCENT)s; width: 12px; height: 12px;
                             margin: -5px 0; border-radius: 6px; }
""" % {"BG": BG, "PANEL": PANEL, "PANEL2": PANEL2, "BORDER": BORDER, "TEXT": TEXT,
       "MUTED": MUTED, "ACCENT": ACCENT, "GREEN": GREEN, "YELLOW": YELLOW,
       "RED": RED, "PURPLE": PURPLE}
