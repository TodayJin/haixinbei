"""protocol.py 单元测试：只依赖标准库。

运行：
    cd tools/host && python -m unittest discover -s tests -t . -v
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import protocol as P


class TestCRC(unittest.TestCase):
    def test_standard_check_value(self):
        # CRC16/CCITT-FALSE 的标准校验值：\"123456789\" -> 0x29B1
        self.assertEqual(P.crc16_ccitt(b"123456789"), 0x29B1)

    def test_empty(self):
        self.assertEqual(P.crc16_ccitt(b""), 0xFFFF)

    def test_incremental_matches_oneshot(self):
        a, b = b"hello ", b"world"
        self.assertEqual(P.crc16_ccitt(b"hello world"),
                         P.crc16_ccitt(b, P.crc16_ccitt(a)))


class TestFrame(unittest.TestCase):
    def test_roundtrip(self):
        f = P.Frame(type=P.T_CMD, payload=b"\x01\x02\x03", seq=7)
        raw = f.to_bytes()
        got = P.parse_all(raw)
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0].type, P.T_CMD)
        self.assertEqual(got[0].seq, 7)
        self.assertEqual(got[0].payload, b"\x01\x02\x03")

    def test_wire_layout(self):
        raw = P.Frame(P.T_ACK, b"\xaa\xbb", 3).to_bytes()
        self.assertEqual(raw[:2], P.SOF)
        self.assertEqual(raw[2], P.T_ACK)
        self.assertEqual(raw[3], 3)
        self.assertEqual(raw[4], 2)
        self.assertEqual(raw[5:7], b"\xaa\xbb")
        self.assertEqual(len(raw), 7 + 2)

    def test_empty_payload(self):
        raw = P.Frame(P.T_PARAM_SAVE, b"", 0).to_bytes()
        self.assertEqual(len(raw), P.FRAME_OVERHEAD)
        self.assertEqual(P.parse_all(raw)[0].payload, b"")

    def test_payload_too_long_rejected(self):
        with self.assertRaises(ValueError):
            P.Frame(P.T_TEXT, b"x" * (P.MAX_PAYLOAD + 1)).to_bytes()

    def test_empty_frame_is_7_bytes(self):
        self.assertEqual(len(P.Frame(P.T_HELLO_REQ).to_bytes()), P.FRAME_OVERHEAD)


class TestParser(unittest.TestCase):
    def setUp(self):
        self.frames = [
            P.Frame(P.T_TELEM, P.Telemetry(t_ms=i, yaw_x10=i * 10).to_bytes(), i & 0xFF)
            for i in range(1, 6)
        ]
        self.stream = b"".join(f.to_bytes() for f in self.frames)

    def test_all_at_once(self):
        got = P.parse_all(self.stream)
        self.assertEqual(len(got), 5)
        self.assertEqual(P.Telemetry.from_bytes(got[3].payload).t_ms, 4)

    def test_byte_by_byte(self):
        """串口按 1 字节到达也必须能解出全部帧。"""
        parser = P.FrameParser()
        out = []
        for b in self.stream:
            out.extend(parser.feed(bytes([b])))
        self.assertEqual(len(out), 5)
        self.assertEqual(parser.stats.crc_errors, 0)

    def test_chunked(self):
        parser = P.FrameParser()
        out = []
        for i in range(0, len(self.stream), 7):
            out.extend(parser.feed(self.stream[i:i + 7]))
        self.assertEqual(len(out), 5)

    def test_leading_garbage_resync(self):
        parser = P.FrameParser()
        out = parser.feed(b"\x00\xff\x12\xa5junk" + self.stream)
        self.assertEqual(len(out), 5)
        self.assertGreater(parser.stats.resyncs, 0)

    def test_corrupt_crc_drops_one_frame_and_recovers(self):
        bad = bytearray(self.frames[2].to_bytes())
        bad[8] ^= 0xFF                      # 破坏载荷
        parser = P.FrameParser()
        out = parser.feed(self.frames[0].to_bytes() + bytes(bad) + self.frames[3].to_bytes())
        self.assertEqual([f.seq for f in out], [1, 4])   # 中间那帧被丢弃
        self.assertEqual(parser.stats.crc_errors, 1)

    def test_absurd_length_resyncs(self):
        parser = P.FrameParser()
        junk = P.SOF + bytes([P.T_TELEM, 0, 0xFF]) + b"\x00" * 4
        out = parser.feed(junk + self.stream)
        self.assertGreaterEqual(parser.stats.length_errors, 1)
        self.assertEqual(len(out), 5)

    def test_partial_frame_waits(self):
        parser = P.FrameParser()
        raw = self.frames[0].to_bytes()
        self.assertEqual(parser.feed(raw[:5]), [])
        got = parser.feed(raw[5:])
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0].seq, 1)

    def test_noise_only_produces_nothing(self):
        parser = P.FrameParser()
        self.assertEqual(parser.feed(bytes(range(256))), [])
        self.assertEqual(parser.stats.frames, 0)


class TestTelemetry(unittest.TestCase):
    def test_size_is_32(self):
        self.assertEqual(P.TELEM_STRUCT.size, 32)
        self.assertEqual(len(P.Telemetry().to_bytes()), 32)

    def test_roundtrip_with_negatives(self):
        t = P.Telemetry(t_ms=123456, yaw_x10=-1799, pitch_x10=-123, roll_x10=456,
                        v_l_mmps=-1500, v_r_mmps=1500, pwm_l=-7200, pwm_r=7200,
                        vbat_mv=11800, ibat_ma=-2300, servo1_us=1500, servo2_us=2500,
                        line_mask=0b10110100, state_id=17, loop_us=210, flags=0x23)
        got = P.Telemetry.from_bytes(t.to_bytes())
        self.assertEqual(got, t)
        self.assertAlmostEqual(got.yaw, -179.9)
        self.assertAlmostEqual(got.vbat, 11.8)
        self.assertAlmostEqual(got.ibat, -2.3)

    def test_line_bits_order_is_left_to_right(self):
        t = P.Telemetry(line_mask=0b00000001)
        self.assertEqual(t.line_bits, [1, 0, 0, 0, 0, 0, 0, 0])
        self.assertLess(t.line_error, 0)            # 最左边 -> 负

    def test_line_error_center_is_zero(self):
        t = P.Telemetry(line_mask=0b00011000)       # 中间两路
        self.assertAlmostEqual(t.line_error, 0.0)

    def test_line_error_lost_is_zero(self):
        self.assertEqual(P.Telemetry(line_mask=0).line_error, 0.0)

    def test_flags(self):
        t = P.Telemetry(flags=P.F_GYRO_OK | P.F_STUCK)
        self.assertIn("陀螺正常", t.flag_list)
        self.assertIn("疑似卡死", t.flag_list)

    def test_wrong_length_rejected(self):
        with self.assertRaises(ValueError):
            P.Telemetry.from_bytes(b"\x00" * 31)


class TestParams(unittest.TestCase):
    def test_ids_unique(self):
        ids = [p.pid for p in P.PARAMS]
        self.assertEqual(len(ids), len(set(ids)))

    def test_keys_unique(self):
        keys = [p.key for p in P.PARAMS]
        self.assertEqual(len(keys), len(set(keys)))

    def test_default_in_range(self):
        for p in P.PARAMS:
            self.assertGreaterEqual(p.default, p.vmin, p.key)
            self.assertLessEqual(p.default, p.vmax, p.key)

    def test_code_defaults_match_firmware(self):
        """参数表里的默认值必须与固件源码一致，防止两边漂移。
        循迹环增益是旧板值 ×7.2（旧满量程 1000 → 本板 7200，PID 输出是 PWM 偏置），
        积分限幅不折算（量纲是误差本身）—— 见 app_ui.c 的 g_cfg 注释（m02112 同步）。
        **角度环是例外**：2026-10-01 原地实测后重定，150/15（旧刻度换算值 720/72 自激）。"""
        self.assertEqual(P.PARAM_BY_KEY["line_kp"].default, 2700.0)   # pid.c:15（旧 375 ×7.2）
        self.assertEqual(P.PARAM_BY_KEY["line_ki"].default, 10.8)     # pid.c:16（旧 1.5 ×7.2）
        self.assertEqual(P.PARAM_BY_KEY["line_kd"].default, 360.0)    # pid.c:17（旧 50 ×7.2）
        self.assertEqual(P.PARAM_BY_KEY["line_ilimit"].default, 50.0)  # pid.c:29（不折算）
        self.assertEqual(P.PARAM_BY_KEY["gyro_kp"].default, 150.0)   # 实测选定（720 自激）
        self.assertEqual(P.PARAM_BY_KEY["gyro_ki"].default, 0.0)      # 默认 0：积分被静摩擦死区喂成常驻推力，车停在带外抽
        self.assertEqual(P.PARAM_BY_KEY["gyro_kd"].default, 15.0)    # ≈Kp/10，压过冲
        self.assertEqual(P.PARAM_BY_KEY["base_speed"].default, 6480.0)  # host_link.c SPECS / app_ui.c trace_speed（旧 900）
        self.assertEqual(P.PARAM_BY_KEY["base_speed_slow"].default, 2500.0)  # host_link.c SPECS（故意不折算，1440 在死区下）
        self.assertEqual(P.PARAM_BY_KEY["task_speed"].default, 6480.0)  # 比赛流程 旧板 900 的换算基准
        self.assertEqual(P.PARAM_BY_KEY["pwm_max"].default, 7200.0)  # bsp_board.h:28
        self.assertEqual(P.PARAM_BY_KEY["counts_per_rev"].default, 1980.0)

    def test_table_crc_is_stable(self):
        self.assertEqual(P.param_table_crc(), P.param_table_crc())

    def test_table_matches_firmware_source(self):
        """id / 默认值 / 顺序直接解析固件 host_link.h + host_link.c 核对。

        以前这份核对是 out/ 下手抄的一份 FW 列表，改了默认值忘了同步它、脚本自己撒谎
        （2026-09-30 增益 ×7.2），所以搬到这里由源码驱动。"""
        import check_param_table as C

        fw, _host, problems = C.compare()
        self.assertGreater(len(fw), 20)
        self.assertEqual(problems, [], "固件参数表与上位机参数表不一致：%s" % "；".join(problems))
        self.assertIsInstance(P.param_table_crc(), int)

    def test_param_get_encoding(self):
        raw = P.build_param_get(0x0101, seq=9)
        f = P.parse_all(raw)[0]
        self.assertEqual(f.type, P.T_PARAM_GET)
        self.assertEqual(struct.unpack("<H", f.payload)[0], 0x0101)

    def test_param_set_roundtrip(self):
        raw = P.build_param_set(0x0203, 66.5, seq=4)
        f = P.parse_all(raw)[0]
        pid, val = struct.unpack("<Hf", f.payload)
        self.assertEqual(pid, 0x0203)
        self.assertAlmostEqual(val, 66.5, places=4)

    def test_param_get_all_uses_ffff(self):
        f = P.parse_all(P.build_param_get())[0]
        self.assertEqual(struct.unpack("<H", f.payload)[0], 0xFFFF)

    def test_groups_nonempty(self):
        self.assertGreaterEqual(len(P.group_names()), 4)


class TestCommands(unittest.TestCase):
    def test_cmd_encoding_including_negative_arg(self):
        f = P.parse_all(P.build_cmd(P.CMD_SET_TELEM_HZ, -50, seq=1))[0]
        cmd, arg = struct.unpack("<Bi", f.payload)
        self.assertEqual(cmd, P.CMD_SET_TELEM_HZ)
        self.assertEqual(arg, -50)

    def test_ack_parse(self):
        f = P.parse_all(P.Frame(P.T_ACK, bytes([7, P.ST_OK]), 0).to_bytes())[0]
        self.assertEqual(P.parse_ack(f.payload), (7, P.ST_OK))

    def test_ack_wrong_len(self):
        with self.assertRaises(ValueError):
            P.parse_ack(b"\x01")

    def test_every_command_has_a_name(self):
        for c in (P.CMD_STOP, P.CMD_START_A, P.CMD_START_B, P.CMD_START_C,
                  P.CMD_CALIB_GYRO, P.CMD_SAVE_PARAMS, P.CMD_CLEAR_FAULT,
                  P.CMD_SET_TELEM_HZ, P.CMD_RESET, P.CMD_SET_LOGGING):
            self.assertIn(c, P.CMD_NAMES)


class TestHelloAndText(unittest.TestCase):
    def test_hello_roundtrip(self):
        h = P.Hello(fw_version=0x0102, proto_version=P.PROTO_VERSION,
                    param_crc=P.param_table_crc(), telem_hz=100, name="haixin-car")
        got = P.Hello.from_bytes(h.to_bytes())
        self.assertEqual(got.fw_version, 0x0102)
        self.assertEqual(got.param_crc, P.param_table_crc())
        self.assertEqual(got.telem_hz, 100)
        self.assertEqual(got.name, "haixin-car")

    def test_hello_long_name_truncated(self):
        h = P.Hello(name="x" * 40)
        self.assertEqual(len(P.Hello.from_bytes(h.to_bytes()).name), 12)

    def test_hello_too_short(self):
        with self.assertRaises(ValueError):
            P.Hello.from_bytes(b"\x00" * 4)

    def test_text_utf8_and_gbk(self):
        self.assertEqual(P.decode_text("陀螺正常".encode("utf-8")), "陀螺正常")
        self.assertEqual(P.decode_text("陀螺正常".encode("gbk")), "陀螺正常")


if __name__ == "__main__":
    unittest.main(verbosity=2)
