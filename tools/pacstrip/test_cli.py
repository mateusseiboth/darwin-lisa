import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_macho import BRAA_X16_X17, BR_X16, LDRAB_OUT_OF_RANGE, RET, RETAB, thin, words_at

CLI = Path(__file__).with_name("pacstrip.py")


def run(*args):
    return subprocess.run([sys.executable, "-I", str(CLI), *map(str, args)], capture_output=True, text=True)


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        data, self.text = thin([RETAB, BRAA_X16_X17, LDRAB_OUT_OF_RANGE])
        self.path = Path(self.tmp.name, "bin")
        self.path.write_bytes(data)

    def test_scan_reports_and_changes_nothing(self):
        before = self.path.read_bytes()
        out = run("scan", self.path)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("2 to patch, 1 to emulate", out.stdout)
        self.assertIn(f"{self.text + 8:#x} {LDRAB_OUT_OF_RANGE:#010x}", out.stdout)
        self.assertEqual(self.path.read_bytes(), before)

    def test_strip_writes_the_output(self):
        dest = Path(self.tmp.name, "out")
        out = run("strip", self.path, dest)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(words_at(dest.read_bytes(), self.text, 2), [RET, BR_X16])

    def test_strip_in_place_without_output(self):
        run("strip", self.path)
        self.assertEqual(words_at(self.path.read_bytes(), self.text, 1), [RET])

    def test_scan_reads_a_dyld_shared_cache(self):
        from test_dyldcache import VM_PROT_EXECUTE, VM_PROT_READ, cache
        self.path.write_bytes(cache([(0x1000, VM_PROT_READ | VM_PROT_EXECUTE, [RETAB, RETAB, LDRAB_OUT_OF_RANGE])]))
        out = run("scan", self.path)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("2 to patch, 1 to emulate", out.stdout)

    def test_rejects_a_file_that_is_not_mach_o(self):
        self.path.write_bytes(b"\x7fELF" + b"\0" * 60)
        out = run("scan", self.path)
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("not a 64-bit Mach-O", out.stderr)


if __name__ == "__main__":
    unittest.main()
