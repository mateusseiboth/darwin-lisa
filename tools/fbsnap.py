#!/usr/bin/env python3
"""Turn the panel snapshots in a kernel log (boot-arg fb_snap=N, MiniIOMobileFramebuffer.cpp) into PNG files.

    tools/fbsnap.py LOG [OUTDIR]     writes OUTDIR/fbsnap-<n>.png for every complete snapshot in LOG

A snapshot is a line "fbsnap: W H" followed by H lines "fbsnap R <W pixels as rrggbb>": one panel pixel in every 24
in each direction. Each pixel is drawn as a 4x4 block so the picture can be looked at.
"""
import os
import re
import struct
import sys
import zlib

SCALE = 4


def png(path, width, height, rows):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def snapshots(text):
    head = re.compile(r"fbsnap: (\d+) (\d+)")
    line_re = re.compile(r"fbsnap (\d+) ([0-9a-f]+)")
    current = None
    for line in text.splitlines():
        m = head.search(line)
        if m:
            current = {"w": int(m.group(1)), "h": int(m.group(2)), "rows": {}}
            continue
        m = line_re.search(line)
        if m and current is not None:
            current["rows"][int(m.group(1))] = bytes.fromhex(m.group(2)[: current["w"] * 6])
            if len(current["rows"]) == current["h"]:
                yield current
                current = None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    text = open(sys.argv[1], "rb").read().decode("latin-1")
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(out, exist_ok=True)
    n = 0
    for snap in snapshots(text):
        w, h = snap["w"], snap["h"]
        rows = []
        for y in range(h):
            row = snap["rows"].get(y, b"\x00" * (w * 3)).ljust(w * 3, b"\x00")
            wide = b"".join(row[x * 3 : x * 3 + 3] * SCALE for x in range(w))
            rows.extend([wide] * SCALE)
        path = os.path.join(out, f"fbsnap-{n}.png")
        png(path, w * SCALE, h * SCALE, rows)
        print(path)
        n += 1
    if n == 0:
        print("no complete snapshot in", sys.argv[1])


if __name__ == "__main__":
    main()
