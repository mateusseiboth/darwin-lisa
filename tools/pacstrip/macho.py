"""Strip pointer authentication from the code of a Mach-O file, in place.

Thin 64-bit files and fat files are handled; only arm64 slices are touched
(arm64e is a subtype of CPU_TYPE_ARM64). In each slice, the sections marked
as holding instructions are rewritten word by word with insn.rewrite(),
skipping the ranges LC_DATA_IN_CODE lists (jump tables and literal pools that
could look like an instruction).

The code signature is not refreshed here: the kernel runs these binaries
with code signing off, or a later step re-signs them.
"""
import bisect
import struct
from dataclasses import dataclass, field

from insn import PATCH, TRAP, rewrite

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_DATA_IN_CODE = 0x29
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400
CODE_SECTION = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS


@dataclass
class Report:
    patched: int = 0
    traps: list = field(default_factory=list)  # (file offset, word)

    def add(self, other):
        self.patched += other.patched
        self.traps += other.traps


def load_commands(data, base):
    ncmds, = struct.unpack_from("<I", data, base + 16)
    offset = base + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, offset)
        yield cmd, offset
        offset += size


def code_sections(data, base, cmd_offset):
    nsects, = struct.unpack_from("<I", data, cmd_offset + 64)
    for i in range(nsects):
        section = cmd_offset + 72 + 80 * i
        size, file_offset = struct.unpack_from("<QI", data, section + 40)
        flags, = struct.unpack_from("<I", data, section + 64)
        if flags & CODE_SECTION and file_offset:
            yield base + file_offset, size


def data_in_code(data, base, cmd_offset):
    """The listed ranges, as file offsets."""
    dataoff, datasize = struct.unpack_from("<II", data, cmd_offset + 8)
    for entry in range(base + dataoff, base + dataoff + datasize, 8):
        offset, length, _kind = struct.unpack_from("<IHH", data, entry)
        yield base + offset, base + offset + length


class DataRanges:
    """Sorted, non-overlapping [start, end) ranges with a binary search."""

    def __init__(self, ranges):
        self.ranges = sorted(r for r in ranges if r[1] > r[0])
        self.starts = [start for start, _ in self.ranges]

    def __contains__(self, offset):
        i = bisect.bisect_right(self.starts, offset) - 1
        return i >= 0 and offset < self.ranges[i][1]


def strip_section(data, start, size, ranges):
    report = Report()
    for offset in range(start, start + size - 3, 4):
        if offset in ranges:
            continue
        word, = struct.unpack_from("<I", data, offset)
        status, new = rewrite(word)
        if status == PATCH:
            struct.pack_into("<I", data, offset, new)
            report.patched += 1
        if status == TRAP:
            report.traps.append((offset, word))
    return report


def strip_thin(data, base):
    report = Report()
    cputype, = struct.unpack_from("<i", data, base + 4)
    if cputype != CPU_TYPE_ARM64:
        return report
    commands = list(load_commands(data, base))
    ranges = DataRanges(r for cmd, at in commands if cmd == LC_DATA_IN_CODE for r in data_in_code(data, base, at))
    sections = [s for cmd, at in commands if cmd == LC_SEGMENT_64 for s in code_sections(data, base, at)]
    for start, size in sections:
        report.add(strip_section(data, start, size, ranges))
    return report


def fat_slices(data, magic):
    nfat, = struct.unpack_from(">I", data, 4)
    entry_size, layout = (32, ">iiQQII") if magic == FAT_MAGIC_64 else (20, ">iiIII")
    for i in range(nfat):
        _cputype, _subtype, offset, _size, *_ = struct.unpack_from(layout, data, 8 + entry_size * i)
        yield offset


def strip_fat(data, magic):
    report = Report()
    for offset in fat_slices(data, magic):
        report.add(strip_thin(data, offset))
    return report


def strip(data):
    """Rewrite `data` (a bytearray) in place; return a Report."""
    if struct.unpack_from("<I", data, 0)[0] == MH_MAGIC_64:
        return strip_thin(data, 0)
    magic, = struct.unpack_from(">I", data, 0)
    if magic in (FAT_MAGIC, FAT_MAGIC_64):
        return strip_fat(data, magic)
    raise ValueError("not a 64-bit Mach-O or fat file")
