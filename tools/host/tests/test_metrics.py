"""量化指标的单元测试（纯函数，不需要界面）。"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import metrics as M


class TestMetrics(unittest.TestCase):
    def test_empty_is_safe(self):
        s = M.summarize([])
        self.assertEqual(s["n"], 0)
        self.assertEqual(M.format_metrics(s), "—")

    def test_basic_stats(self):
        s = M.summarize([0, 2, -2, 4])
        self.assertEqual(s["n"], 4)
        self.assertEqual(s["min"], -2)
        self.assertEqual(s["max"], 4)
        self.assertEqual(s["pp"], 6)
        self.assertAlmostEqual(s["mean"], 1.0)

    def test_stdev_single_sample(self):
        self.assertEqual(M.stdev([5]), 0.0)

    def test_zero_crossings_counts_oscillation(self):
        self.assertEqual(M.zero_crossings([1, -1, 1, -1]), 3)
        self.assertEqual(M.zero_crossings([1, 1, 1]), 0)
        self.assertEqual(M.zero_crossings([]), 0)

    def test_zero_crossings_deadband_ignores_noise(self):
        noisy = [1, 0.01, -0.01, 1, -0.02, 1]
        self.assertEqual(M.zero_crossings(noisy, deadband=0.05), 0)
        self.assertGreater(M.zero_crossings(noisy, deadband=0.0), 0)

    def test_settling_time(self):
        xs = [0, 1, 2, 3, 4, 5]
        ys = [0, 5, 5, 0.1, 0.1, 0.1]
        self.assertAlmostEqual(M.settling_time(xs, ys, 0.0, 0.5), 3.0)

    def test_settling_time_never_settles(self):
        self.assertIsNone(M.settling_time([0, 1, 2, 3], [5, 5, 5, 5], 0.0, 0.5))

    def test_overshoot(self):
        self.assertAlmostEqual(M.overshoot([0, 1.2, 1.0, 1.0], target=1.0), 0.2)

    def test_format_contains_numbers(self):
        txt = M.format_metrics(M.summarize([0, 1, -1]), unit="mm/s")
        self.assertIn("峰峰", txt)
        self.assertIn("过零", txt)
        self.assertIn("mm/s", txt)


if __name__ == "__main__":
    unittest.main(verbosity=2)
