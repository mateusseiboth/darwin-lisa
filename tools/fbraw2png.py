#!/usr/bin/env python3
"""fbraw2png.py RAW OUT.png WIDTH HEIGHT STRIDE: the loader's "oem screenshot" (BGRX rows) as a PNG."""
import struct
import sys
import zlib

raw, out, width, height, stride = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:6])
data = open(raw, "rb").read()
rows = bytearray()
for y in range(height):
    line = data[y * stride : y * stride + width * 4]
    rows.append(0)
    rgb = bytearray(width * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
    rows += rgb


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


with open(out, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))
