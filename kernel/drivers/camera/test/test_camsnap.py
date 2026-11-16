#!/usr/bin/env python3
"""camsnap.py: the kernel's "camsnap" lines (hakios.camera.c frame_snap()) back into a picture."""
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import camsnap  # noqa: E402


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    at, idat, size = 8, b"", None
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            size = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    width, height = size
    raw = zlib.decompress(idat)
    rows = [raw[y * (width * 3 + 1) + 1:(y + 1) * (width * 3 + 1)] for y in range(height)]
    return width, height, rows


def lines_for(cols, rows, pixel, frame=8, noise=True):
    out = []
    for row in range(rows):
        hexrow = "".join(pixel(col, row) for col in range(cols))
        for part, at in enumerate(range(0, len(hexrow), 192)):
            prefix = "[  41.123456] " if noise else ""
            out.append(f"{prefix}camsnap {frame} {cols}x{rows} {row} {part} {hexrow[at:at + 192]}")
    return out


class Camsnap(unittest.TestCase):
    def test_parts_are_joined_and_rows_ordered(self):
        log = lines_for(64, 48, lambda c, r: f"{c * 4:02x}{r * 5:02x}80")
        log.reverse()                   # (any order in the log)
        log.insert(3, "camera: frame 8: R 1 G 2/3 B 4")
        snaps = camsnap.parse(log)
        self.assertEqual(list(snaps), [8])
        width, height, pixels = snaps[8]
        self.assertEqual((width, height), (64, 48))
        self.assertEqual(pixels[47][63], (252, 235, 128))
        self.assertEqual(pixels[0][1], (4, 0, 128))

    def test_png_written_scaled(self):
        log = lines_for(4, 3, lambda c, r: "ff0000" if c == r else "000000", frame=2, noise=False)
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "snap.png")
            written = camsnap.main(["camsnap.py", "-", out, "--scale", "2"], stdin=log)
            self.assertEqual(written, [out])
            width, height, rows = read_png(out)
            self.assertEqual((width, height), (8, 6))
            self.assertEqual(rows[0][:6], bytes([255, 0, 0, 255, 0, 0]))
            self.assertEqual(rows[0][6:9], bytes([0, 0, 0]))

    def test_incomplete_frame_is_refused(self):
        log = lines_for(64, 48, lambda c, r: "101010")[:-3]
        self.assertEqual(camsnap.parse(log), {})

    def test_several_frames_each_their_own_file(self):
        log = lines_for(4, 3, lambda c, r: "010203", frame=1) + lines_for(4, 3, lambda c, r: "040506", frame=9)
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "s.png")
            written = camsnap.main(["camsnap.py", "-", out], stdin=log)
            self.assertEqual(sorted(os.path.basename(p) for p in written), ["s-1.png", "s-9.png"])


def pack_raw10(pixels):
    out = bytearray()
    for i in range(0, len(pixels), 4):
        group = pixels[i:i + 4]
        out += bytes(v >> 2 for v in group) + bytes([sum((v & 3) << (2 * k) for k, v in enumerate(group))])
    return bytes(out)


class Camraw(unittest.TestCase):
    """camraw lines: a full-resolution centre crop of the frame, its RAW10 bytes as they are."""

    def lines(self, frame=24):
        # a GRBG crop 8 x 4: green 512, red 256, blue 128
        out = []
        for row in range(4):
            pixels = [512 if (row % 2 == 0) == (x % 2 == 0) else (256 if row % 2 == 0 else 128) for x in range(8)]
            data = pack_raw10(pixels).hex()
            for part, at in enumerate(range(0, len(data), 8)):
                out.append(f"camraw {frame} 8x4 1 {row} {part} {data[at:at + 8]}")
        return out

    def test_crop_is_unpacked_and_debayered(self):
        crops = camsnap.parse_raw(self.lines())
        width, height, bayer, rows = crops[24]
        self.assertEqual((width, height, bayer), (8, 4, 1))
        self.assertEqual(camsnap.unpack_raw10(rows[0], 8)[:2], [512, 256])
        self.assertEqual(camsnap.debayer(width, height, bayer, rows)[0][0], (64, 128, 32))

    def test_raw_png_written(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "s.png")
            written = camsnap.main(["camsnap.py", "-", out], stdin=self.lines())
            self.assertEqual([os.path.basename(p) for p in written], ["s-crop-24.png"])
            self.assertEqual(read_png(written[0])[:2], (4, 2))
            with open(os.path.join(d, "s-crop-24.raw10"), "rb") as f:
                self.assertEqual(len(f.read()), 4 * 10)


if __name__ == "__main__":
    unittest.main()
