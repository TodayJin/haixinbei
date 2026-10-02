"""端到端测试：无硬件把整条链路跑通。

覆盖：握手 -> 读全部参数 -> 改参数 -> 发命令 -> 收遥测 -> 注入故障
      -> CSV 录制/回放 -> 链路统计。
"""

import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import protocol as P
import recorder as R
from device import Device
from simulator import DemoSource
from transport import Link, LoopbackTransport, SimTransport

FAST = 20.0          # 演示数据倍速：1 秒墙钟 ≈ 20 秒数据时间


def wait_for(cond, timeout=6.0, interval=0.01):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if cond():
            return True
        time.sleep(interval)
    return False


class DeviceTestCase(unittest.TestCase):
    """每个用例开一个演示数据链路，跑完关掉。"""

    def setUp(self):
        self.dev = Device()
        self.status: list[str] = []
        self.telem: list[P.Telemetry] = []
        self.acks: list[tuple[str, int]] = []
        self.dev.subscribe("status", self.status.append)
        self.dev.subscribe("telemetry", self.telem.append)
        self.dev.subscribe("ack", lambda d, s: self.acks.append((d, s)))
        self.dev.open_sim(speed=FAST)

    def tearDown(self):
        self.dev.close()

    def sim(self) -> DemoSource:
        return self.dev.transport.sim


class TestHandshake(DeviceTestCase):
    def test_hello(self):
        self.assertTrue(wait_for(lambda: self.dev.hello is not None))
        h = self.dev.hello
        self.assertEqual(h.name, "demo-signal")
        self.assertEqual(h.proto_version, P.PROTO_VERSION)
        self.assertEqual(h.param_crc, P.param_table_crc())
        self.assertTrue(self.dev.state.param_crc_ok)

    def test_all_params_read_back(self):
        self.assertTrue(wait_for(lambda: self.dev.hello is not None))
        self.assertTrue(wait_for(
            lambda: abs(self.dev.params[P.PARAM_BY_KEY["line_kp"].pid] - 2700.0) < 1e-6))
        for spec in P.PARAMS:
            self.assertAlmostEqual(self.dev.params[spec.pid], spec.default, places=4,
                                   msg=spec.key)

    def test_telemetry_flows(self):
        self.assertTrue(wait_for(lambda: len(self.telem) > 30))
        t = self.telem[-1]
        self.assertGreaterEqual(t.vbat_mv, 9000)
        self.assertGreaterEqual(t.ibat_ma, -5000)
        self.assertTrue(0 <= t.line_mask <= 0xFF)


class TestParamPath(DeviceTestCase):
    def test_set_param_reaches_source_and_echoes(self):
        self.assertTrue(wait_for(lambda: self.dev.hello is not None))
        ok, msg = self.dev.set_param("line_kp", 4000.0)
        self.assertTrue(ok, msg)
        spec = P.PARAM_BY_KEY["line_kp"]
        self.assertTrue(wait_for(lambda: abs(self.sim().params[spec.pid] - 4000.0) < 1e-3))
        self.assertTrue(wait_for(lambda: any(s == P.ST_OK for _, s in self.acks)))
        self.assertAlmostEqual(self.dev.params[spec.pid], 4000.0, places=3)

    def test_out_of_range_rejected_locally(self):
        self.assertTrue(wait_for(lambda: self.dev.hello is not None))
        before = self.sim().params[P.PARAM_BY_KEY["line_kp"].pid]
        ok, msg = self.dev.set_param("line_kp", 99999.0)
        self.assertFalse(ok)
        self.assertIn("超范围", msg)
        time.sleep(0.05)
        self.assertEqual(self.sim().params[P.PARAM_BY_KEY["line_kp"].pid], before)

    def test_unknown_key_rejected(self):
        ok, msg = self.dev.set_param("no_such_param", 1.0)
        self.assertFalse(ok)
        self.assertIn("未知参数", msg)

    def test_push_all_params(self):
        self.assertTrue(wait_for(lambda: self.dev.hello is not None))
        self.assertEqual(self.dev.push_all_params(), len(P.PARAMS))

    def test_preset_roundtrip(self):
        self.assertTrue(wait_for(lambda: self.dev.hello is not None))
        self.dev.set_param("gyro_kd", 12.5)
        path = os.path.join(tempfile.gettempdir(), "haixin_preset_%d.json" % os.getpid())
        try:
            self.dev.save_preset(path)
            dev2 = Device()
            n, warn = dev2.load_preset(path)
            self.assertEqual(n, len(P.PARAMS))
            self.assertEqual(warn, [])
            self.assertAlmostEqual(dev2.params[P.PARAM_BY_KEY["gyro_kd"].pid], 12.5, places=3)
        finally:
            if os.path.exists(path):
                os.remove(path)

    def test_preset_with_unknown_key_warns(self):
        path = os.path.join(tempfile.gettempdir(), "haixin_bad_%d.json" % os.getpid())
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write('{"param_crc": 999, "values": {"ghost": 1.0, "gyro_kp": 111.0}}')
            dev = Device()
            n, warn = dev.load_preset(path)
            self.assertEqual(n, 1)
            self.assertTrue(any("ghost" in w for w in warn))
            self.assertTrue(any("指纹" in w for w in warn))
        finally:
            if os.path.exists(path):
                os.remove(path)


class TestCommands(DeviceTestCase):
    def test_start_command_makes_data_move(self):
        self.assertTrue(wait_for(lambda: self.dev.hello is not None))
        self.assertEqual(self.sim().v_l, 0.0)
        self.assertTrue(self.dev.send_cmd(P.CMD_START_A))
        self.assertTrue(wait_for(lambda: self.sim().started))
        self.assertTrue(wait_for(lambda: abs(self.sim().v_l) > 1.0), "数据源没动")

    def test_stop_command(self):
        self.assertTrue(self.dev.send_cmd(P.CMD_START_A))
        self.assertTrue(wait_for(lambda: self.sim().started))
        self.assertTrue(self.dev.send_cmd(P.CMD_STOP))
        self.assertTrue(wait_for(lambda: not self.sim().started))

    def test_set_telem_hz(self):
        self.assertTrue(self.dev.set_telem_hz(100))
        self.assertTrue(wait_for(lambda: self.sim().telem_hz == 100))

    def test_gyro_flag_and_voltage(self):
        self.assertTrue(self.dev.send_cmd(P.CMD_START_A))
        self.assertTrue(wait_for(lambda: any(t.flags & P.F_GYRO_OK for t in self.telem)))
        self.assertTrue(wait_for(lambda: any(t.vbat_mv > 10000 for t in self.telem)))

    def test_stuck_flag_and_current(self):
        """注入的堵转必须体现为 STUCK 标志 + 电流飙升。"""
        self.assertTrue(self.dev.send_cmd(P.CMD_START_A))
        self.assertTrue(wait_for(lambda: any(t.flags & P.F_STUCK for t in self.telem), 9.0))
        stuck = [t for t in self.telem if t.flags & P.F_STUCK]
        self.assertGreater(max(t.ibat_ma for t in stuck), 2000)


class TestRecording(DeviceTestCase):
    def test_record_then_replay(self):
        self.assertTrue(self.dev.send_cmd(P.CMD_START_A))
        path = os.path.join(tempfile.gettempdir(), "haixin_rec_%d.csv" % os.getpid())
        try:
            self.dev.start_recording(path)
            self.assertTrue(wait_for(lambda: len(self.telem) > 60))
            n = self.dev.stop_recording()
            self.assertGreater(n, 40)
            frames = R.read_csv(path)
            self.assertEqual(len(frames), n)
            s = R.summarize(frames)
            self.assertEqual(s.n, n)
            self.assertGreater(s.duration_s, 0)
            self.assertGreater(s.v_max, 0)
        finally:
            if os.path.exists(path):
                os.remove(path)

    def test_replay_transport_reproduces_frames(self):
        self.assertTrue(self.dev.send_cmd(P.CMD_START_A))
        path = os.path.join(tempfile.gettempdir(), "haixin_rec2_%d.csv" % os.getpid())
        try:
            self.dev.start_recording(path)
            self.assertTrue(wait_for(lambda: len(self.telem) > 60))
            self.dev.stop_recording()
            original = R.read_csv(path)
        finally:
            if os.path.exists(path):
                os.remove(path)

        from transport import ReplayTransport
        got: list[P.Telemetry] = []
        tr = ReplayTransport(original, speed=0.0)
        link = Link(tr, on_frame=lambda f: got.append(P.Telemetry.from_bytes(f.payload)))
        link.open()
        try:
            self.assertTrue(wait_for(lambda: len(got) >= len(original), 5.0))
        finally:
            link.close()
        self.assertEqual(len(got), len(original))
        self.assertEqual(got[0].t_ms, original[0].t_ms)
        self.assertEqual(got[-1].t_ms, original[-1].t_ms)


class TestLinkMechanics(unittest.TestCase):
    def test_loopback_roundtrip(self):
        got: list[P.Frame] = []
        link = Link(LoopbackTransport(), on_frame=got.append)
        link.open()
        try:
            self.assertTrue(link.send(P.Frame(P.T_CMD, b"\x01\x00\x00\x00\x00", 5)))
            self.assertTrue(wait_for(lambda: len(got) == 1))
            self.assertEqual(got[0].type, P.T_CMD)
            self.assertEqual(got[0].seq, 5)
        finally:
            link.close()

    def test_stats_and_no_crc_errors_over_demo(self):
        dev = Device()
        try:
            dev.open_sim(speed=FAST)
            self.assertTrue(wait_for(lambda: dev.state.frames > 100))
            st = dev.link.stats
            self.assertGreater(st.frames_in, 100)
            self.assertGreater(st.bytes_in, 1000)
            self.assertEqual(st.crc_errors, 0)
            self.assertIn("CRC", dev.link_stats_text())
        finally:
            dev.close()

    def test_seq_wraps(self):
        link = Link(LoopbackTransport())
        link._seq = 254
        self.assertEqual([link.next_seq() for _ in range(4)], [255, 0, 1, 2])


class TestDemoSourceBehaviour(unittest.TestCase):
    """演示数据源的行为 —— 它要能体现"改参数看得出来"。"""

    def _run(self, kp, seconds=20.0, dt=0.005):
        sim = DemoSource()
        sim.params[P.PARAM_BY_KEY["line_kp"].pid] = kp
        sim._do_cmd(P.CMD_START_A, 0)
        errs, masks = [], set()
        for _ in range(int(seconds / dt)):
            sim.tick(dt)
            errs.append(abs(sim.line_err))
            masks.add(sim.line_mask)
        return errs, masks

    def test_higher_kp_shrinks_line_error(self):
        loose, _ = self._run(60)
        tight, _ = self._run(3000)
        self.assertGreater(sum(loose) / len(loose), sum(tight) / len(tight),
                           "Kp 调大后循迹偏差应当变小（在线调参的意义）")

    def test_mask_follows_line_error(self):
        _, masks = self._run(2700)
        self.assertGreater(len(masks), 1, "灰度掩码一直不变，灰度条就是死的")

    def test_deterministic(self):
        a, _ = self._run(2700, 10.0)
        b, _ = self._run(2700, 10.0)
        self.assertEqual(a, b)

    def test_flags_and_stuck_injection(self):
        sim = DemoSource()
        sim._do_cmd(P.CMD_START_A, 0)
        seen_stuck = False
        for _ in range(int(30.0 / 0.005)):
            sim.tick(0.005)
            if sim.telemetry().flags & P.F_STUCK:
                seen_stuck = True
        self.assertTrue(seen_stuck, "注入的堵转未出现")

    def test_never_goes_out_of_protocol_range(self):
        sim = DemoSource()
        sim._do_cmd(P.CMD_START_A, 0)
        for _ in range(int(30.0 / 0.005)):
            sim.tick(0.005)
            t = sim.telemetry()
            self.assertGreaterEqual(t.pwm_l, -7200)
            self.assertLessEqual(t.pwm_l, 7200)
            self.assertLessEqual(abs(t.yaw_x10), 1800)
            self.assertTrue(0 <= t.line_mask <= 0xFF)
            t.to_bytes()

    def test_no_pose_frame_anymore(self):
        """位姿/得分帧已随地图一起删掉。"""
        self.assertFalse(hasattr(P, "T_POSE"))
        self.assertFalse(hasattr(P, "Pose"))
        self.assertFalse(hasattr(DemoSource(), "pose"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
