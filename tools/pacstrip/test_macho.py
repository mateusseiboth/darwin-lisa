import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from macho import strip

RETAB = 0xD65F0FFF
RET = 0xD65F03C0
BRAA_X16_X17 = 0xD71F0A11
BR_X16 = 0xD61F0200
LDRAB_OUT_OF_RANGE = 0xF8BFFC41  # ldrab x1, [x2, #4088]!
NOP = 0xD503201F

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_X86_64 = 0x01000007
CPU_SUBTYPE_ARM64E = 2
LC_SEGMENT_64 = 0x19
LC_DATA_IN_CODE = 0x29
S_ATTR_PURE_INSTRUCTIONS = 0x80000000

CLANG = shutil.which("clang")
OBJDUMP = shutil.which("llvm-objdump")


def thin(words, data_in_code=(), cputype=CPU_TYPE_ARM64, section_flags=S_ATTR_PURE_INSTRUCTIONS):
    """A minimal 64-bit Mach-O: one __TEXT,__text section holding `words`,
    and a LC_DATA_IN_CODE listing (offset, length) ranges from the header."""
    header_size = 32
    segment_size = 72 + 80
    dic_cmd_size = 16
    text_offset = header_size + segment_size + dic_cmd_size
    text = b"".join(struct.pack("<I", w) for w in words)
    dic_offset = text_offset + len(text)
    dic = b"".join(struct.pack("<IHH", off, length, 1) for off, length in data_in_code)
    header = struct.pack("<IiiIIIII", MH_MAGIC_64, cputype, CPU_SUBTYPE_ARM64E, 1, 2,
                         segment_size + dic_cmd_size, 0, 0)
    segment = struct.pack("<II16sQQQQiiII", LC_SEGMENT_64, segment_size, b"__TEXT", 0, len(text),
                          text_offset, len(text), 5, 5, 1, 0)
    section = struct.pack("<16s16sQQIIIIIIII", b"__text", b"__TEXT", 0, len(text), text_offset, 2,
                          0, 0, section_flags, 0, 0, 0)
    dic_cmd = struct.pack("<IIII", LC_DATA_IN_CODE, dic_cmd_size, dic_offset, len(dic))
    return bytearray(header + segment + section + dic_cmd + text + dic), text_offset


def words_at(data, offset, count):
    return list(struct.unpack_from(f"<{count}I", data, offset))


def fat(*slices):
    """A fat file with each thin slice aligned to 2^14."""
    align = 1 << 14
    head = struct.pack(">II", 0xCAFEBABE, len(slices))
    offset = align
    entries, body = b"", b""
    for data, cputype in slices:
        entries += struct.pack(">iiIII", cputype, CPU_SUBTYPE_ARM64E, offset, len(data), 14)
        padded = bytes(data) + b"\0" * (-len(data) % align)
        body += padded
        offset += len(padded)
    first = head + entries
    return bytearray(first + b"\0" * (align - len(first)) + body)


class ThinFiles(unittest.TestCase):
    def test_rewrites_the_text_section(self):
        data, text = thin([RETAB, NOP, BRAA_X16_X17])
        report = strip(data)
        self.assertEqual(words_at(data, text, 3), [RET, NOP, BR_X16])
        self.assertEqual(report.patched, 2)
        self.assertEqual(report.traps, [])

    def test_reports_traps_by_file_offset(self):
        data, text = thin([NOP, LDRAB_OUT_OF_RANGE])
        report = strip(data)
        self.assertEqual(report.traps, [(text + 4, LDRAB_OUT_OF_RANGE)])
        self.assertEqual(words_at(data, text + 4, 1), [LDRAB_OUT_OF_RANGE])

    def test_leaves_data_in_code_alone(self):
        _, text = thin([RETAB, RETAB, RETAB])
        data, text = thin([RETAB, RETAB, RETAB], data_in_code=[(text + 4, 4)])
        strip(data)
        self.assertEqual(words_at(data, text, 3), [RET, RETAB, RET])

    def test_leaves_sections_without_instructions_alone(self):
        data, text = thin([RETAB], section_flags=0)
        report = strip(data)
        self.assertEqual(words_at(data, text, 1), [RETAB])
        self.assertEqual(report.patched, 0)

    def test_leaves_other_architectures_alone(self):
        data, text = thin([RETAB], cputype=CPU_TYPE_X86_64)
        strip(data)
        self.assertEqual(words_at(data, text, 1), [RETAB])

    def test_rejects_what_is_not_mach_o(self):
        with self.assertRaises(ValueError):
            strip(bytearray(b"\x7fELF" + b"\0" * 60))


class FatFiles(unittest.TestCase):
    def test_rewrites_only_the_arm64_slices(self):
        arm, arm_text = thin([RETAB])
        intel, intel_text = thin([RETAB], cputype=CPU_TYPE_X86_64)
        data = fat((arm, CPU_TYPE_ARM64), (intel, CPU_TYPE_X86_64))
        report = strip(data)
        self.assertEqual(words_at(data, (1 << 14) + arm_text, 1), [RET])
        self.assertEqual(words_at(data, 2 * (1 << 14) + intel_text, 1), [RETAB])
        self.assertEqual(report.patched, 1)


@unittest.skipUnless(CLANG and OBJDUMP, "clang or llvm-objdump is not installed")
class CompiledObject(unittest.TestCase):
    SOURCE = """
    struct ops { int (*fn)(int); void *(*get)(void); };
    int call(struct ops *o, int v) { return o->fn(v) + 1; }
    void *get(struct ops *o) { return o->get(); }
    int deep(struct ops *o, int n) { return n ? deep(o, n - 1) + call(o, n) : 0; }
    """
    LEFTOVER = ("braa", "brab", "blraa", "blrab", "retaa", "retab", "ldraa", "ldrab",
                "pacia ", "pacib ", "pacda ", "pacdb ", "autia ", "autib ", "autda ", "autdb ", "pacga")

    def test_no_pointer_authentication_is_left(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, obj = Path(tmp, "t.c"), Path(tmp, "t.o")
            src.write_text(self.SOURCE)
            subprocess.run([CLANG, "-target", "arm64e-apple-ios17.0", "-O2", "-c", str(src), "-o", str(obj)], check=True)
            before = subprocess.run([OBJDUMP, "-d", "--macho", str(obj)], capture_output=True, text=True).stdout
            self.assertRegex(before, r"\bretab\b")
            data = bytearray(obj.read_bytes())
            report = strip(data)
            obj.write_bytes(data)
            after = subprocess.run([OBJDUMP, "-d", "--macho", str(obj)], capture_output=True, text=True).stdout
        self.assertGreater(report.patched, 0)
        for mnemonic in self.LEFTOVER:
            with self.subTest(mnemonic=mnemonic):
                self.assertNotIn(f"\t{mnemonic}", after)


if __name__ == "__main__":
    unittest.main()
