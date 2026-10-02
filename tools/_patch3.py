import pathlib
p = pathlib.Path("tools/host/tests/test_ui_qt.py")
s = p.read_text(encoding="utf-8")
s = s.replace('["赛道", "波形", "参数", "命令", "记录", "日志"]',
              '["波形", "参数", "参数总览", "命令", "记录", "日志"]')
s = s.replace('        self.assertEqual(self.win.ob_list.count(), 11)\n', '')
s = s.replace('        self.assertIsNotNone(self.win._last_pose, "没收到位姿")\n', '')
s = s.replace('        self.assertNotEqual(self.win.score._v["score"].text(), "-")\n', '')
s = s.replace('''    def test_running_race_accumulates_score(self):
        self.connect_sim()
        self.win.dev.send_cmd(P.CMD_START_A)
        self.pump(4.0)
        p = self.win._last_pose
        self.assertGreaterEqual(p.score, 10, "5 分钟里至少该过一次障碍")''',
'''    def test_param_overview_renders(self):
        self.connect_sim()
        rows = self.win.pbars.rows
        self.assertEqual(len(rows), len(P.PARAMS))
        img = self.win.pbars.grab()
        self.assertFalse(img.isNull())
        self.assertGreater(img.height(), len(P.PARAMS) * 10)''')
s = s.replace('''    def test_map_and_dial_repaint(self):
        self.connect_sim()
        self.win.dev.send_cmd(P.CMD_START_A)
        self.pump(2.0)
        self.assertGreater(len(self.win.map.trail), 3, "地图上没画出轨迹")
        img = self.win.map.grab()
        self.assertGreater(img.width(), 100)
        self.assertFalse(img.isNull())''',
'''    def test_dial_and_sensors_repaint(self):
        self.connect_sim()
        self.win.dev.send_cmd(P.CMD_START_A)
        self.pump(2.0)
        for w in (self.win.dial, self.win.sensors, self.win.speeds):
            self.assertFalse(w.grab().isNull())''')
s = s.replace('self.win.mode.setCurrentText("模拟器")', 'self.win.mode.setCurrentText("演示数据")')
s = s.replace('self.assertIn("模拟器", self.win.status.text())', 'self.assertIn("演示数据", self.win.status.text())')
p.write_text(s, encoding="utf-8")
print("test_ui_qt.py 已更新")

p = pathlib.Path("tools/host/tests/test_ui_smoke.py")
s = p.read_text(encoding="utf-8")
s = s.replace('self.app.mode.set("模拟器")', 'self.app.mode.set("演示数据")')
p.write_text(s, encoding="utf-8")
print("test_ui_smoke.py 已更新")
