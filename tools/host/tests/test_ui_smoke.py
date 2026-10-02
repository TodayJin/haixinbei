"""界面冒烟测试：不需要人看，也不用真的串口。

做法：把窗口摆到屏幕外（+4000+4000）后真实布局，然后手动泵事件循环，
检查"遥测是否驱动了控件"。没有图形环境时整体跳过。

运行：
    cd tools/host && python tests\test_ui_smoke.py
"""

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import protocol as P

try:
    import tkinter as tk
    _probe = tk.Tk()
    _probe.withdraw()
    _probe.destroy()
    HAVE_DISPLAY = True
except Exception as exc:                              # pragma: no cover
    HAVE_DISPLAY = False
    DISPLAY_ERR = str(exc)


@unittest.skipUnless(HAVE_DISPLAY, "没有图形环境，跳过界面测试")
class TestUiSmoke(unittest.TestCase):
    def setUp(self):
        import app as A
        self.app = A.HostApp()
        self.app.geometry("1180x740+4000+4000")       # 屏幕外，别打扰用户
        self.app.update()

    def tearDown(self):
        # 走真正的关闭路径（同时验证它不会留下悬挂的 after 回调）
        try:
            self.app._on_close()
        except Exception:
            self.app.destroy()

    def pump(self, seconds: float) -> None:
        t0 = time.monotonic()
        while time.monotonic() - t0 < seconds:
            self.app.update()
            time.sleep(0.01)

    def connect_sim(self, speed: str = "20"):
        self.app.mode.set("模拟器")
        self.app.speed_var.set(speed)
        self.app.toggle_connect()

    # -------------------------------------------------- 用例
    def test_window_builds(self):
        self.assertEqual(self.app.title(), "全地形机器人上位机")
        self.assertGreaterEqual(len(self.app.tree.get_children()), len(P.PARAMS))
        self.assertEqual(len(self.app.wave_vars), len(self.app.wave_channels()))

    def test_simulator_drives_all_widgets(self):
        self.connect_sim()
        self.pump(2.5)
        self.assertTrue(self.app.dev.connected)
        self.assertIsNotNone(self.app._last_telem, "没收到遥测")
        self.assertGreater(self.app.dev.state.frames, 20)

        t = self.app._last_telem
        # 灰度阵列画出来了吗
        self.assertGreater(len(self.app.sensors.find_all()), 8)
        # 波形有线条
        self.assertGreater(len(self.app.wave.find_all()), 0)
        # 表盘与速度条
        self.assertGreater(len(self.app.dial.find_all()), 10)
        self.assertGreater(len(self.app.speeds.find_all()), 5)
        # 状态格被填上了
        self.assertNotEqual(self.app.info.values["state"].cget("text"), "-")
        self.assertIn("V", self.app.info.values["vbat"].cget("text"))
        # 状态栏有链路统计
        self.assertIn("模式", self.app.status.cget("text"))
        self.assertIn("帧", self.app.status.cget("text"))
        self.assertEqual(P.Telemetry.from_bytes(t.to_bytes()), t)

    def test_disconnect_and_reconnect(self):
        self.connect_sim()
        self.pump(0.6)
        self.app.toggle_connect()                      # 断开
        self.pump(0.3)
        self.assertFalse(self.app.dev.connected)
        self.assertIn("未连接", self.app.conn_dot.cget("text"))
        self.app.toggle_connect()                      # 再连
        self.pump(0.8)
        self.assertTrue(self.app.dev.connected)

    def test_param_row_updates_after_set(self):
        self.connect_sim()
        self.pump(1.0)
        ok, msg = self.app.dev.set_param("gyro_kd", 12.5)
        self.assertTrue(ok, msg)
        self.pump(0.4)
        self.app._refresh_param_row(P.PARAM_BY_KEY["gyro_kd"].pid)
        vals = self.app.tree.item(str(P.PARAM_BY_KEY["gyro_kd"].pid), "values")
        self.assertEqual(vals[1], "12.5")

    def test_restore_defaults_refills_table(self):
        self.connect_sim()
        self.pump(0.6)
        self.app.dev.set_param("line_kd", 99.0)
        self.app._restore_defaults()
        vals = self.app.tree.item(str(P.PARAM_BY_KEY["line_kd"].pid), "values")
        self.assertEqual(float(vals[1]), P.PARAM_BY_KEY["line_kd"].default)

    def test_wave_channel_toggle_and_window(self):
        self.connect_sim()
        self.pump(1.0)
        self.app.wave_vars["pwm_l"].set(True)
        self.app._apply_wave_visible()
        self.app.win_var.set("5")
        self.app.wave.set_window(5.0)
        self.pump(0.6)
        self.assertIn("pwm_l", self.app.wave.visible)
        self.assertEqual(self.app.wave.window_s, 5.0)
        self.assertGreater(len(self.app.wave.find_all()), 0)

    def test_recording_from_ui_and_analyze(self):
        """确定性地测录制链路：直接喂帧，不依赖实时速度（跑快了会偶发为 0）。"""
        import tempfile
        import recorder as R
        from protocol import Telemetry
        self.connect_sim(speed="1")
        self.pump(0.4)
        path = os.path.join(tempfile.gettempdir(), "haixin_ui_rec_%d.csv" % os.getpid())
        try:
            self.app.dev.start_recording(path)
            for i in range(40):
                self.app.dev._on_telem(Telemetry(t_ms=i * 20, yaw_x10=i * 10,
                                                 vbat_mv=12000, ibat_ma=500,
                                                 line_mask=0x18, v_l_mmps=70))
            n = self.app.dev.stop_recording()
            self.assertEqual(n, 40)
            self.assertFalse(self.app.dev.recording)
            frames = R.read_csv(path)
            self.assertEqual(len(frames), 40)
            self.assertEqual(R.summarize(frames).n, 40)
        finally:
            if os.path.exists(path):
                os.remove(path)

    def test_log_page_receives_status(self):
        self.connect_sim()
        self.pump(1.0)
        text = self.app.log.get("1.0", "end")
        self.assertIn("已连接", text)


if __name__ == "__main__":
    if not HAVE_DISPLAY:                              # pragma: no cover
        print("没有图形环境：%s" % DISPLAY_ERR)
    unittest.main(verbosity=2)
