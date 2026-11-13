"""Tests of tools/scroll_window.py: python3 -I -m unittest discover -s darwin-lisa/tools -p 'test_scroll_window.py'"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import scroll_window  # noqa: E402

LOG = """\
dlisa[backboardd]: scroll: 8 strokes from uptime 250.000 s
dlisa[Preferences]: probe 2.0 s at uptime 252 s: touches 120 (began 1 moved 118 ended 1 cancelled 0), runloop 90 turns, busiest 15 ms; display link 0 calls (paused)
dlisa[backboardd]: scroll: 8 strokes from uptime 250.000 to 266.500 s (150 moves 8 ms apart, held 160 ms, each)
[  245.000000]: MiniIOMobileFramebuffer: 2 frames in 5000 ms (0 a second), 1500 us each
[  250.1000000]: MiniIOMobileFramebuffer: 10 frames in 5000 ms (2 a second), 1500 us each
[  256.000000]: MiniIOMobileFramebuffer: 150 frames in 5000 ms (30 a second), 1500 us each
[  261.000000]: MiniIOMobileFramebuffer: 140 frames in 5000 ms (28 a second), 1500 us each
[  266.000000]: MiniIOMobileFramebuffer: 145 frames in 5000 ms (29 a second), 1500 us each
[  271.000000]: MiniIOMobileFramebuffer: 20 frames in 5000 ms (4 a second), 1500 us each
[  268.560769]: goodix: so far 3 reports in 11250 looks (0 bad), 0 finger positions, the last at 0, 0
"""


class ParseTest(unittest.TestCase):
    def test_kernel_stamp_with_seven_digits_is_a_carried_second(self):
        # the kernel prints 127.007797 as "126.1007797"
        self.assertAlmostEqual(scroll_window.parse_stamp("126", "1007797"), 127.007797)
        self.assertAlmostEqual(scroll_window.parse_stamp("268", "560769"), 268.560769)

    def test_finds_the_scroll_window(self):
        self.assertEqual(scroll_window.find_scroll(LOG.splitlines()), (8, 250.0, 266.5))

    def test_no_scroll_line_no_window(self):
        self.assertIsNone(scroll_window.find_scroll(["nothing here"]))

    def test_framebuffer_windows_keep_their_span(self):
        windows = scroll_window.find_frame_windows(LOG.splitlines())
        self.assertEqual(windows[0], (240.0, 245.0, 2))
        self.assertAlmostEqual(windows[1][1], 251.0)


class VerdictTest(unittest.TestCase):
    def test_windows_wholly_inside_the_strokes_decide(self):
        report = scroll_window.judge(LOG.splitlines(), min_fps=15)
        # 251-256, 256-261 and 261-266 lie inside 250-266.5; 246-251 and 266-271 only overlap it
        self.assertEqual([w[2] for w in report["inside"]], [150, 140, 145])
        self.assertEqual(len(report["overlapping"]), 5)
        self.assertTrue(report["passed"])
        self.assertAlmostEqual(report["fps_inside"], 29.0)

    def test_a_stalled_window_inside_fails(self):
        lines = LOG.replace("140 frames in 5000 ms", "3 frames in 5000 ms").splitlines()
        report = scroll_window.judge(lines, min_fps=15)
        self.assertFalse(report["passed"])

    def test_no_window_inside_cannot_pass(self):
        lines = [l for l in LOG.splitlines() if "MiniIOMobileFramebuffer" not in l]
        self.assertFalse(scroll_window.judge(lines, min_fps=15)["passed"])

    def test_probe_lines_in_the_window_are_kept(self):
        report = scroll_window.judge(LOG.splitlines(), min_fps=15)
        self.assertEqual(len(report["probe"]), 1)
        self.assertIn("touches 120", report["probe"][0])


if __name__ == "__main__":
    unittest.main()
