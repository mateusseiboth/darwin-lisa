#!/usr/bin/env python3
"""The camera driver's thumbnails out of a boot log, as PNG pictures.

hakios.camera.c (cam=4) writes the last frame it captured into the log as lines
  camsnap <frame> <cols>x<rows> <row> <part> <hex>
each <hex> at most 32 pixels of RRGGBB (one 2x2 Bayer cell each, 8 bits a colour). This puts the rows back together.
With cam_crop=W it also writes a full-resolution centre crop of that frame as it came from the sensor:
  camraw <frame> <w>x<h> <bayer> <row> <part> <hex>
<hex> the RAW10 bytes of the row (four pixels in five bytes). Those become OUT-crop-<frame>.raw10 (the rows one after
the other, w * 5 / 4 bytes each) and OUT-crop-<frame>.png (each 2x2 cell one pixel, 8 bits a colour, no balance).

  python3 -I camsnap.py LOG OUT.png [--scale N]     (LOG "-" for standard input; several frames: OUT-<frame>.png)
"""
import re
import struct
import sys
import zlib

LINE = re.compile(r"camsnap (\d+) (\d+)x(\d+) (\d+) (\d+) ([0-9a-f]+)")
RAW = re.compile(r"camraw (\d+) (\d+)x(\d+) (\d) (\d+) (\d+) ([0-9a-f]+)")
# where red, the two greens and blue sit in a 2x2 cell (raster order) for each order (0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR)
SITES = {0: (0, 1, 2, 3), 1: (1, 0, 3, 2), 2: (2, 0, 3, 1), 3: (3, 1, 2, 0)}


def parse(lines):
    """{frame: (width, height, rows of (r, g, b))} for every frame whose rows are all there."""
    parts = {}
    for line in lines:
        found = LINE.search(line)
        if found:
            frame, cols, rows, row, part = (int(found.group(i)) for i in range(1, 6))
            parts.setdefault((frame, cols, rows), {}).setdefault(row, {})[part] = found.group(6)
    snaps = {}
    for (frame, cols, rows), by_row in parts.items():
        pixels = []
        for row in range(rows):
            text = "".join(by_row.get(row, {}).get(p, "") for p in sorted(by_row.get(row, {})))
            if len(text) != cols * 6:
                break
            pixels.append([tuple(int(text[i + k:i + k + 2], 16) for k in (0, 2, 4)) for i in range(0, len(text), 6)])
        if len(pixels) == rows:
            snaps[frame] = (cols, rows, pixels)
    return snaps


def parse_raw(lines):
    """{frame: (width, height, bayer, rows of RAW10 bytes)} for every crop whose rows are all there."""
    parts = {}
    for line in lines:
        found = RAW.search(line)
        if found:
            frame, w, h, bayer, row, part = (int(found.group(i)) for i in range(1, 7))
            parts.setdefault((frame, w, h, bayer), {}).setdefault(row, {})[part] = found.group(7)
    crops = {}
    for (frame, w, h, bayer), by_row in parts.items():
        rows = []
        for row in range(h):
            text = "".join(by_row.get(row, {}).get(p, "") for p in sorted(by_row.get(row, {})))
            if len(text) != w * 5 // 4 * 2:
                break
            rows.append(bytes.fromhex(text))
        if len(rows) == h:
            crops[frame] = (w, h, bayer, rows)
    return crops


def unpack_raw10(line, width):
    out = []
    for i in range(0, width, 4):
        group = line[i // 4 * 5:i // 4 * 5 + 5]
        out += [group[k] << 2 | (group[4] >> (2 * k) & 3) for k in range(4)]
    return out


def debayer(width, height, bayer, rows):
    """Each 2x2 cell one RGB pixel, 8 bits a colour."""
    red, green_a, green_b, blue = SITES[bayer]
    out = []
    for y in range(0, height - 1, 2):
        top, bottom = unpack_raw10(rows[y], width), unpack_raw10(rows[y + 1], width)
        line = []
        for x in range(0, width - 1, 2):
            cell = (top[x], top[x + 1], bottom[x], bottom[x + 1])
            line.append((cell[red] >> 2, (cell[green_a] + cell[green_b]) >> 3, cell[blue] >> 2))
        out.append(line)
    return out


def write_png(path, width, height, pixels, scale):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = bytearray()
    for row in pixels:
        line = b"".join(bytes(p) * scale for p in row)
        for _ in range(scale):
            raw += b"\0" + line
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def main(argv, stdin=None):
    scale = 1
    if "--scale" in argv:
        at = argv.index("--scale")
        scale = max(1, int(argv[at + 1]))
        argv = argv[:at] + argv[at + 2:]
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return []
    if argv[1] == "-":
        lines = stdin if stdin is not None else sys.stdin
    else:
        with open(argv[1], errors="replace") as f:
            lines = f.readlines()
    lines = list(lines)
    snaps = parse(lines)
    written = []
    for frame, (width, height, pixels) in sorted(snaps.items()):
        out = argv[2] if len(snaps) == 1 else re.sub(r"(\.png)?$", f"-{frame}.png", argv[2], count=1)
        write_png(out, width, height, pixels, scale)
        written.append(out)
        print(f"camsnap.py: frame {frame}: {width}x{height} -> {out}")
    for frame, (width, height, bayer, rows) in sorted(parse_raw(lines).items()):
        base = re.sub(r"(\.png)?$", f"-crop-{frame}", argv[2], count=1)
        with open(base + ".raw10", "wb") as f:
            f.write(b"".join(rows))
        pixels = debayer(width, height, bayer, rows)
        write_png(base + ".png", width // 2, height // 2, pixels, scale)
        written.append(base + ".png")
        print(f"camsnap.py: frame {frame}: crop {width}x{height} (bayer {bayer}) -> {base}.png, {base}.raw10")
    if not written:
        print("camsnap.py: no complete camsnap frame or camraw crop in the log", file=sys.stderr)
    return written


if __name__ == "__main__":
    sys.exit(0 if main(sys.argv) else 1)
