"""PySide6 界面冒烟测试（离屏，不需要显示器）。

用 QT_QPA_PLATFORM=offscreen 跑；离屏平台没有字体，所以只断言**结构**不断言像素。
另外会真的 grab() 一张 PNG 出来，确认渲染管线是通的 —— 这个测试抓到过
"paintEvent 里 QPen 没导入"这类只在运行时才炸的问题。

运行：
    cd tools/host && python tests\test_ui_qt.py
"""

import os
import sys
import time
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import protocol as P

try:
    from PySide6.QtWidgets import QApplication
    HAVE_QT = True
except ImportError:                                   # pragma: no cover
    HAVE_QT = False


TAB_NAMES = ["波形", "参数", "参数总览", "命令", "记录", "日志"]


@unittest.skipUnless(HAVE_QT, "未安装 PySide6，跳过 Qt 界面测试")
class TestQtUi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qapp = QApplication.instance() or QApplication([])
        from ui_qt import theme as T
        cls.qapp.setStyleSheet(T.QSS)

    def setUp(self):
        from ui_qt.window import MainWindow
        # load_config=False：测试不能被上次保存的 config.json 影响
        self.win = MainWindow(load_config=False)
        self.win.resize(1360, 860)
        self.win.show()
        self.pump(0.2)

    def tearDown(self):
        self.win.hide()
        self.win.close()

    def pump(self, seconds: float) -> None:
        t0 = time.monotonic()
        while time.monotonic() - t0 < seconds:
            self.qapp.processEvents()
            time.sleep(0.008)

    def connect_demo(self, speed="20"):
        self.win.select_mode("演示数据")
        self.win.speed.setCurrentText(speed)
        self.win.toggle_connect()
        self.pump(0.5)

    # ------------------------------------------------------------ 用例
    def test_window_structure(self):
        self.assertEqual(self.win.tabs.count(), len(TAB_NAMES))
        self.assertEqual([self.win.tabs.tabText(i) for i in range(len(TAB_NAMES))],
                         TAB_NAMES)
        self.assertEqual(self.win.table.rowCount(), len(P.PARAMS))
        self.assertEqual(len(self.win.pbars.rows), len(P.PARAMS))
        self.assertEqual([self.win.mode.itemText(i) for i in range(self.win.mode.count())],
                         ["演示数据", "串口", "回放"])

    def test_select_mode_guard(self):
        """组合框里没有的模式要返回 False，而不是静默无效。"""
        self.assertTrue(self.win.select_mode("演示数据"))
        self.assertFalse(self.win.select_mode("不存在的模式"))

    def test_demo_source_updates_widgets(self):
        self.connect_demo()
        self.pump(3.0)
        self.assertTrue(self.win.dev.connected)
        self.assertIsNotNone(self.win._last_telem, "没收到遥测")
        self.assertGreater(self.win.dev.state.frames, 30)
        self.assertIn("V", self.win.info._v["vbat"].text())
        self.assertNotEqual(self.win.info._v["state"].text(), "-")
        self.assertIn("演示数据", self.win.status.text())
        self.assertIn("帧", self.win.status.text())

    def test_gauges_repaint(self):
        self.connect_demo()
        self.win.dev.send_cmd(P.CMD_START_A)
        self.pump(2.0)
        for w in (self.win.dial, self.win.sensors, self.win.speeds, self.win.pbars):
            self.assertFalse(w.grab().isNull(), w.__class__.__name__)

    def test_wave_lanes_follow_checkboxes(self):
        self.connect_demo()
        self.assertEqual(len(self.win.wave_curves), 4)          # 默认勾 4 个
        self.win.wave_vars["vbat"].setChecked(True)
        self.pump(0.3)
        self.assertEqual(len(self.win.wave_curves), 5)
        self.win.wave_vars["yaw"].setChecked(False)
        self.pump(0.3)
        self.assertEqual(len(self.win.wave_curves), 4)
        self.assertNotIn("yaw", self.win.wave_curves)

    def test_wave_gets_data(self):
        self.connect_demo()
        self.win.dev.send_cmd(P.CMD_START_A)
        self.pump(2.0)
        times, data = self.win.dev.snapshot()
        self.assertGreater(len(times), 20)
        self.assertGreater(max(abs(v) for v in data["v_l"]), 0.0)

    def test_param_edit_path(self):
        self.connect_demo()
        self.pump(0.8)
        ok, msg = self.win.dev.set_param("gyro_kd", 12.5)
        self.assertTrue(ok, msg)
        self.pump(0.4)
        self.win._refresh_param_row(P.PARAM_BY_KEY["gyro_kd"].pid)
        found = None
        for i in range(self.win.table.rowCount()):
            if self.win.table.item(i, 0).text() == P.PARAM_BY_KEY["gyro_kd"].name:
                found = self.win.table.item(i, 1).text()
        self.assertEqual(found, "12.5")

    def test_connect_never_pops_a_dialog(self):
        """连接流程不许弹模态框（以前选到"回放"会一直弹，用户点一次弹一次）。"""
        self.win.select_mode("回放")
        self.win.toggle_connect()          # 不能阻塞在对话框上
        self.pump(0.5)
        self.assertTrue(self.win.dev.connected)
        self.assertEqual(self.win.mode.currentText(), "演示数据")
        text = self.win.log.toPlainText()
        self.assertIn("回放模式", text)

    def test_baseline_store_and_clear(self):
        self.connect_demo()
        self.win.dev.send_cmd(P.CMD_START_A)
        self.pump(2.0)
        self.win._store_baseline()
        self.assertGreater(len(self.win.baseline), 0)
        key = next(iter(self.win.baseline))
        self.assertGreater(len(self.win.baseline[key][1]), 2)
        self.win._clear_baseline()
        self.assertEqual(self.win.baseline, {})

    def test_metrics_label_updates(self):
        self.connect_demo()
        self.win.dev.send_cmd(P.CMD_START_A)
        self.pump(2.0)
        self.win._update_metrics()
        text = self.win.metrics_lbl.text()
        self.assertIn("峰峰", text)
        self.assertIn("过零", text)

    def test_param_diff_marks_changed(self):
        self.connect_demo()
        self.pump(0.8)
        spec = P.PARAM_BY_KEY["gyro_kd"]
        self.assertNotIn(spec.name, self.win.pbars.changed)
        self.win.dev.set_param("gyro_kd", spec.default + 3.0)
        self.pump(0.6)        # 走"参数回读 -> 实时刷新"这条真实路径
        self.assertIn(spec.name, self.win.pbars.changed)

    def test_config_roundtrip(self):
        """会话记忆：存了要能读回来。跑完恢复原文件，别污染真实配置。"""
        import json
        from ui_qt.window import CONFIG_PATH, MainWindow
        backup = None
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, "rb") as fh:
                backup = fh.read()
        try:
            self.win.win_sec.setCurrentText("20")
            self.win.wave_vars["vbat"].setChecked(True)
            self.win._save_config()
            self.assertTrue(os.path.exists(CONFIG_PATH))
            with open(CONFIG_PATH, encoding="utf-8") as fh:
                self.assertIn("vbat", json.load(fh)["visible"])
            w2 = MainWindow()
            try:
                self.assertEqual(w2.win_sec.currentText(), "20")
                self.assertTrue(w2.wave_vars["vbat"].isChecked())
            finally:
                w2.hide()
                w2.close()
        finally:
            if backup is None:
                if os.path.exists(CONFIG_PATH):
                    os.remove(CONFIG_PATH)
            else:
                with open(CONFIG_PATH, "wb") as fh:
                    fh.write(backup)

    def test_screenshot_pipeline(self):
        self.connect_demo()
        self.pump(1.5)
        img = self.win.grab()
        self.assertFalse(img.isNull())
        self.assertGreater(img.width(), 800)
        outdir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                              "out", "host")
        os.makedirs(outdir, exist_ok=True)
        path = os.path.join(outdir, "ui_qt_smoke.png")
        self.assertTrue(img.save(path))
        self.assertGreater(os.path.getsize(path), 5000)

    def test_recording_from_ui(self):
        import tempfile
        import recorder as R
        from protocol import Telemetry
        self.connect_demo(speed="1")
        path = os.path.join(tempfile.gettempdir(), "haixin_qt_rec_%d.csv" % os.getpid())
        try:
            self.win.dev.start_recording(path)
            for i in range(30):
                self.win.dev._on_telem(Telemetry(t_ms=i * 20, vbat_mv=12000,
                                                 line_mask=0x18))
            self.assertEqual(self.win.dev.stop_recording(), 30)
            self.assertEqual(len(R.read_csv(path)), 30)
        finally:
            if os.path.exists(path):
                os.remove(path)

    def test_disconnect(self):
        self.connect_demo()
        self.win.toggle_connect()
        self.pump(0.3)
        self.assertFalse(self.win.dev.connected)
        self.assertIn("未连接", self.win.conn_lbl.text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
