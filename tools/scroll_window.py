#!/usr/bin/env python3
"""Frames a second while libdlisa's scripted scroll (boot-arg dlisa_scroll) drags a list, from a run's end log.

usage: python3 -I darwin-lisa/tools/scroll_window.py LOG [--min-fps N]

The window is backboardd's "scroll: N strokes from uptime A to B s" line; the frames are the kernel's timestamped
"MiniIOMobileFramebuffer: N frames in M ms" windows (each ends at its stamp). A 5 s window wholly inside the strokes
counts; the run passes when there is at least one and every one of them has at least --min-fps frames a second
(default 15: planb-standalone's finger scroll was 27). dlisa_uiprobe's lines inside the window are printed as well.
"""
import argparse
import re
import sys

SCROLL = re.compile(r"dlisa\[backboardd\]: scroll: (\d+) strokes from uptime ([\d.]+) to ([\d.]+) s")
FRAMES = re.compile(r"^\[\s*(\d+)\.(\d+)\]: MiniIOMobileFramebuffer: (\d+) frames in (\d+) ms")
PROBE = re.compile(r"dlisa\[[^]]+\]: probe [\d.]+ s at uptime (\d+) s")


def parse_stamp(seconds, fraction):
    """The kernel's "[ S.F]": F is microseconds, printed with a seventh digit when it reaches a whole second."""
    return int(seconds) + int(fraction) / 1e6


def find_scroll(lines):
    for line in lines:
        m = SCROLL.search(line)
        if m:
            return int(m.group(1)), float(m.group(2)), float(m.group(3))
    return None


def find_frame_windows(lines):
    windows = []
    for line in lines:
        m = FRAMES.search(line)
        if m:
            end = parse_stamp(m.group(1), m.group(2))
            windows.append((end - int(m.group(4)) / 1000, end, int(m.group(3))))
    return windows


def judge(lines, min_fps):
    scroll = find_scroll(lines)
    report = {"scroll": scroll, "inside": [], "overlapping": [], "probe": [], "passed": False, "fps_inside": 0.0}
    if scroll is None:
        return report
    _, start, end = scroll
    windows = find_frame_windows(lines)
    report["inside"] = [w for w in windows if w[0] >= start and w[1] <= end]
    report["overlapping"] = [w for w in windows if w[1] > start and w[0] < end]
    report["probe"] = [l.strip() for l in lines if (m := PROBE.search(l)) and start <= int(m.group(1)) <= end + 2]
    rates = [w[2] / (w[1] - w[0]) for w in report["inside"]]
    report["fps_inside"] = sum(w[2] for w in report["inside"]) / sum(w[1] - w[0] for w in report["inside"]) if rates else 0.0
    report["passed"] = bool(rates) and min(rates) >= min_fps
    return report


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log")
    parser.add_argument("--min-fps", type=float, default=15)
    args = parser.parse_args(argv)
    with open(args.log, "rb") as f:
        lines = f.read().decode("latin1").splitlines()
    report = judge(lines, args.min_fps)
    if report["scroll"] is None:
        print("no 'dlisa[backboardd]: scroll: ... to ... s' line: dlisa_scroll did not run (or the run ended first)")
        return 2
    count, start, end = report["scroll"]
    print(f"scroll: {count} strokes, uptime {start:.1f} to {end:.1f} s")
    for w in report["overlapping"]:
        mark = "inside" if w in report["inside"] else "partly"
        print(f"  {w[0]:7.1f}-{w[1]:7.1f} s  {w[2]:4d} frames  {w[2] / (w[1] - w[0]):5.1f}/s  ({mark})")
    for line in report["probe"]:
        print(f"  {line}")
    print(f"frames a second inside the strokes: {report['fps_inside']:.1f}; "
          f"{'PASS' if report['passed'] else 'FAIL'} (every whole window at least {args.min_fps:g}/s)")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
