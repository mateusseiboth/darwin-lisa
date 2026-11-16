"""Tests of tools/frame_bursts.py: python3 -I -m unittest discover -s darwin-lisa/tools -p 'test_frame_bursts.py'"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import frame_bursts  # noqa: E402


def swaps(start_us, gaps_ms, first_id=1):
    """tl: s lines: one at start_us, then one after each gap"""
    lines, t, n = [f"tl: s {first_id} at {start_us}"], start_us, first_id
    for g in gaps_ms:
        t += int(g * 1000)
        n += 1
        lines.append(f"tl: s {n} at {t}")
    return lines


WINDOWS = """\
[   42.785812]: MiniIOMobileFramebuffer: 18 frames in 5008 ms (3 a second), 2319 us each in the driver
MiniIOMobileFramebuffer: from one frame to the next: 0 times under 10 ms, 4 under 18, 0 under 26, 5 under 35, 7 longer (the longest 61 ms); half under 33 ms, 95 in 100 under 62 ms; 1 pauses
HakiosMetal[backboardd 43]: fps: ... gaps between commits while something moves: 93 on time (20 ms at most), 10 a frame late (to 50 ms), 3 stalls (past 50); a buffer made
HakiosMetal[backboardd 43]: fps: ... draw calls by processor: cpu0 1679 at 36 us cpu1 2342 at 29 us cpu6 71 at 78 us (TPIDR_EL0 0x2)
HakiosMetal[SpringBoard 6]: fps: ... draw calls by processor: cpu4 500 at 90 us (TPIDR_EL0 0)
[   47.828536]: MiniIOMobileFramebuffer: 171 frames in 5028 ms (34 a second), 1951 us each in the driver
MiniIOMobileFramebuffer: from one frame to the next: 0 times under 10 ms, 127 under 18, 4 under 26, 9 under 35, 11 longer (the longest 81 ms); half under 17 ms, 95 in 100 under 50 ms; 20 pauses
HakiosMetal[backboardd 43]: fps: ... gaps between commits while something moves: 42 on time (20 ms at most), 12 a frame late (to 50 ms), 11 stalls (past 50); a buffer made
HakiosMetal[backboardd 43]: fps: ... draw calls by processor: cpu0 774 at 23 us cpu5 9 at 26 us (TPIDR_EL0 0x3)
[   52.929441]: MiniIOMobileFramebuffer: 51 frames in 5001 ms (10 a second), 1584 us each in the driver
HakiosMetal[backboardd 43]: fps: ... gaps between commits while something moves: 0 on time (20 ms at most), 0 a frame late (to 50 ms), 21 stalls (past 50); a buffer made
[   58.019957]: MiniIOMobileFramebuffer: 52 frames in 5001 ms (10 a second), 1519 us each in the driver
MiniIOMobileFramebuffer: from one frame to the next: 0 times under 10 ms, 0 under 18, 0 under 26, 1 under 35, 1 longer (the longest 68 ms); half under 34 ms, 95 in 100 under 65 ms; 50 pauses
"""


class BurstTest(unittest.TestCase):
    def test_swaps_closer_than_a_pause_make_one_burst(self):
        lines = swaps(10_000_000, [16.7] * 30 + [100] * 5 + [16.7] * 20)
        bursts = frame_bursts.find_bursts(frame_bursts.find_swaps(lines))
        # 31 swaps, then 100 ms pauses (each alone: no burst), then the last pause's swap starts 21 more
        self.assertEqual([b["swaps"] for b in bursts], [31, 21])
        self.assertAlmostEqual(bursts[0]["start"], 10.0)
        self.assertAlmostEqual(bursts[0]["seconds"], 0.501, places=3)

    def test_burst_intervals_are_sorted_into_the_framebuffer_buckets(self):
        lines = swaps(0, [16] * 10 + [20] * 5 + [40] * 3)
        b = frame_bursts.find_bursts(frame_bursts.find_swaps(lines))[0]
        self.assertEqual((b["under18"], b["to35"], b["longer"]), (10, 5, 3))
        self.assertAlmostEqual(b["median_ms"], 16.0)

    def test_short_flurries_are_not_bursts(self):
        lines = swaps(0, [16] * 5)
        self.assertEqual(frame_bursts.find_bursts(frame_bursts.find_swaps(lines)), [])

    def test_a_burst_is_labelled_with_the_gesture_just_before_it(self):
        lines = ["tl: p 1 9 32768 64880 at 4900000", "tl: p 2 9 32768 63000 at 4916000"]
        lines += swaps(5_200_000, [16] * 20)
        lines += ["tl: p 3 9 1 1 at 9000000"] + swaps(20_000_000, [16] * 20, first_id=40)
        bursts = frame_bursts.label(frame_bursts.find_bursts(frame_bursts.find_swaps(lines)),
                                    frame_bursts.find_gestures(lines))
        self.assertAlmostEqual(bursts[0]["after_gesture"], 4.9)
        self.assertIsNone(bursts[1]["after_gesture"])   # 11 s after the last touch: not that gesture's

    def test_swap_lines_inside_other_text_are_found(self):
        lines = ["junk tl: s 7 at 1000 more", "[  3.000]: tl: s 8 at 17000"]
        self.assertEqual(frame_bursts.find_swaps(lines), [0.001, 0.017])


class WindowTest(unittest.TestCase):
    def test_the_animation_is_the_busiest_window_and_the_one_before(self):
        a = frame_bursts.boot_animation(WINDOWS.splitlines())
        self.assertEqual(a["frames"], (18, 171))
        # the moving intervals of both windows, pauses left out
        self.assertEqual((a["under18"], a["to35"], a["longer"]), (131, 18, 18))
        self.assertEqual(a["moving"], 167)

    def test_commits_and_draws_of_the_animation_windows_are_summed(self):
        a = frame_bursts.boot_animation(WINDOWS.splitlines())
        self.assertEqual(a["commits"], (135, 22, 14))
        # backboardd's draws only: cpu0-3 1679+2342+774, cpu4-6 71+9
        self.assertEqual(a["draws_fast"], 4795)
        self.assertEqual(a["draws_slow"], 80)

    def test_an_idle_block_inside_the_span_adds_no_commits(self):
        idle = ("HakiosMetal[backboardd 43]: fps: ... gaps between commits while something moves: 0 on time (20 ms at most), "
                "0 a frame late (to 50 ms), 21 stalls (past 50); a buffer made")
        lines = WINDOWS.splitlines()
        lines.insert(2, idle)
        self.assertEqual(frame_bursts.boot_animation(lines)["commits"], (135, 22, 14))

    def test_a_later_busier_window_is_not_the_boot_animation(self):
        later = ("[  134.822659]: MiniIOMobileFramebuffer: 180 frames in 5353 ms (33 a second), 1951 us each in the driver\n"
                 "MiniIOMobileFramebuffer: from one frame to the next: 0 times under 10 ms, 150 under 18, 0 under 26, "
                 "0 under 35, 0 longer (the longest 17 ms); half under 17 ms, 95 in 100 under 17 ms; 0 pauses\n")
        a = frame_bursts.boot_animation((WINDOWS + later).splitlines())
        self.assertEqual(a["frames"], (18, 171))

    def test_no_window_no_animation(self):
        self.assertIsNone(frame_bursts.boot_animation(["nothing"]))


if __name__ == "__main__":
    unittest.main()
