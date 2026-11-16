#!/usr/bin/env python3
"""How smooth each animation of a run was, swap by swap, and the boot animation's windows, from end logs.

usage: python3 -I darwin-lisa/tools/frame_bursts.py LOG [LOG ...]

Bursts (needs boot-arg touch_lat=1): the "tl: s ID at US" lines are every swap handed to the panel driver. A burst is a
run of at least 10 swaps each less than 90 ms after the one before (the framebuffer's FRAME_PAUSE: from 90 ms on
nothing was moving). For each: when it began, how long it lasted, swaps a second, and its intervals in the
framebuffer's buckets (under 18 ms, 18-35, longer). A burst that begins within 3 s of a touch ("tl: p" lines; a
gesture is touches less than 0.5 s apart) is that gesture's.

The boot animation (no boot-arg needed): the 5 s "MiniIOMobileFramebuffer: N frames" windows cut the lock-to-home
animation wherever their phase falls, and a 7-core boot reaches it some 8 s earlier than a 4-core one, so one window's
frame count compares two runs badly (S8-A 18 + 171 frames, S8-B 3 + 185, the same 167 and 169 moving intervals; the
animation itself is a burst of ~160 swaps in 3.2 s). This takes the first window of at least 100 frames and the one
before it, and sums their histograms (pauses left out), backboardd's commit gaps (2 s blocks with at least 20 commits
on time or late) and its draws on cpu0-3 (A78) and cpu4-6 (A55), from the window before them to the window after.
"""
import re
import statistics
import sys

PAUSE_MS = 90
MIN_SWAPS = 10
GESTURE_GAP_S = 0.5
GESTURE_REACH_S = 3.0
FAST_CPUS = range(0, 4)
ANIMATION_FRAMES = 100          # the first window with so many frames holds the boot animation
MOVING_COMMITS = 20             # a 2 s block with fewer commits on time or late is the 10 Hz idle redraw

SWAP = re.compile(r"tl: s \d+ at (\d+)")
TOUCH = re.compile(r"tl: p \d+ \d+ \d+ \d+ at (\d+)")
WINDOW = re.compile(r"MiniIOMobileFramebuffer: (\d+) frames in (\d+) ms")
HIST = re.compile(r"from one frame to the next: (\d+) times under 10 ms, (\d+) under 18, (\d+) under 26, (\d+) under 35, "
                  r"(\d+) longer")
COMMITS = re.compile(r"HakiosMetal\[backboardd \d+\]: fps: \.\.\. gaps between commits while something moves: (\d+) on time"
                     r".*?, (\d+) a frame late.*?, (\d+) stalls")
DRAWS = re.compile(r"HakiosMetal\[backboardd \d+\]: fps: \.\.\. draw calls by processor: (.*)")
DRAW = re.compile(r"cpu(\d+) (\d+) at")


def find_swaps(lines):
    return [int(m.group(1)) / 1e6 for line in lines for m in SWAP.finditer(line)]


def find_touches(lines):
    return [int(m.group(1)) / 1e6 for line in lines for m in TOUCH.finditer(line)]


def find_gestures(lines):
    """(first touch, last touch) of each gesture"""
    gestures = []
    for t in find_touches(lines):
        if gestures and t - gestures[-1][1] < GESTURE_GAP_S:
            gestures[-1][1] = t
        else:
            gestures.append([t, t])
    return [tuple(g) for g in gestures]


def describe(times):
    gaps = [(b - a) * 1000 for a, b in zip(times, times[1:])]
    seconds = times[-1] - times[0]
    return {
        "start": times[0],
        "seconds": seconds,
        "swaps": len(times),
        "per_second": (len(times) - 1) / seconds if seconds > 0 else 0.0,
        "under18": sum(g < 18 for g in gaps),
        "to35": sum(18 <= g < 35 for g in gaps),
        "longer": sum(g >= 35 for g in gaps),
        "median_ms": statistics.median(gaps),
        "after_gesture": None,
    }


def find_bursts(swap_times):
    runs, run = [], []
    for t in swap_times:
        if run and (t - run[-1]) * 1000 >= PAUSE_MS:
            runs.append(run)
            run = []
        run.append(t)
    runs.append(run)
    return [describe(r) for r in runs if len(r) >= MIN_SWAPS]


def label(bursts, gestures):
    for b in bursts:
        before = [g for g in gestures if g[0] <= b["start"]]
        if before and b["start"] - before[-1][1] <= GESTURE_REACH_S:
            b["after_gesture"] = before[-1][0]
    return bursts


def find_windows(lines):
    """(line index, frames, histogram buckets or None) of each framebuffer window"""
    windows = []
    for i, line in enumerate(lines):
        m = WINDOW.search(line)
        if not m:
            continue
        hist = None
        for later in lines[i:i + 3]:
            h = HIST.search(later)
            if h:
                hist = tuple(int(x) for x in h.groups())
                break
            if later is not line and WINDOW.search(later):
                break
        windows.append((i, int(m.group(1)), hist))
    return windows


def boot_animation(lines):
    windows = find_windows(lines)
    if not windows:
        return None
    busy = [k for k, w in enumerate(windows) if w[1] >= ANIMATION_FRAMES]
    busiest = busy[0] if busy else max(range(len(windows)), key=lambda k: windows[k][1])
    chosen = windows[max(busiest - 1, 0):busiest + 1]
    hists = [w[2] or (0, 0, 0, 0, 0) for w in chosen]
    first = windows[busiest - 2][0] + 1 if busiest >= 2 else 0
    last = windows[busiest + 1][0] if busiest + 1 < len(windows) else len(lines)
    commits, fast, slow = [0, 0, 0], 0, 0
    for line in lines[first:last]:
        c = COMMITS.search(line)
        if c and int(c.group(1)) + int(c.group(2)) >= MOVING_COMMITS:
            commits = [a + int(b) for a, b in zip(commits, c.groups())]
        d = DRAWS.search(line)
        if d:
            for cpu, n in DRAW.findall(d.group(1)):
                fast += int(n) if int(cpu) in FAST_CPUS else 0
                slow += int(n) if int(cpu) not in FAST_CPUS else 0
    return {
        "frames": tuple(w[1] for w in chosen),
        "under18": sum(h[0] + h[1] for h in hists),
        "to35": sum(h[2] + h[3] for h in hists),
        "longer": sum(h[4] for h in hists),
        "moving": sum(sum(h) for h in hists),
        "commits": tuple(commits),
        "draws_fast": fast,
        "draws_slow": slow,
    }


def report(path):
    with open(path, "rb") as f:
        lines = f.read().decode("latin1").splitlines()
    print(path)
    a = boot_animation(lines)
    if a:
        share = 100 * a["draws_slow"] / max(a["draws_fast"] + a["draws_slow"], 1)
        print(f"  boot animation, windows of {' + '.join(map(str, a['frames']))} frames: {a['moving']} moving intervals, "
              f"<18 {a['under18']} / 18-35 {a['to35']} / longer {a['longer']}; commits on time/late/stall "
              f"{'/'.join(map(str, a['commits']))}; backboardd draws on the A55s {share:.1f} % "
              f"({a['draws_slow']} of {a['draws_fast'] + a['draws_slow']})")
    bursts = label(find_bursts(find_swaps(lines)), find_gestures(lines))
    if not bursts:
        print("  no swap bursts (touch_lat=1 not set?)")
    for b in bursts:
        what = f"gesture at {b['after_gesture']:.1f} s" if b["after_gesture"] is not None else "no gesture"
        print(f"  burst at {b['start']:7.2f} s ({what}): {b['swaps']} swaps in {b['seconds']:.2f} s, "
              f"{b['per_second']:.1f}/s, <18 {b['under18']} / 18-35 {b['to35']} / longer {b['longer']}, "
              f"median {b['median_ms']:.1f} ms")


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    for path in argv:
        report(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
