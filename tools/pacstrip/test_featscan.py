import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from featscan import scan

LLVM_MC = shutil.which("llvm-mc")
OBJDUMP = shutil.which("llvm-objdump")

ASM = """
.text
.globl _f
_f:
    ldapr   x0, [x1]
    sdot    v0.4s, v1.16b, v2.16b
    fjcvtzs w0, d1
    fjcvtzs w2, d3
    fcmla   v0.4s, v1.4s, v2.4s, #90
    ldapur  x0, [x1, #-8]
    cfinv
    add     x0, x0, #1
    ret
"""


@unittest.skipUnless(LLVM_MC and OBJDUMP, "llvm-mc or llvm-objdump is not installed")
class FeatureScan(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        src, self.obj = Path(self.tmp.name, "f.s"), Path(self.tmp.name, "f.o")
        src.write_text(ASM)
        subprocess.run([LLVM_MC, "-triple=arm64-apple-ios", "-mattr=+v8.5a,+complxnum,+jsconv,+rcpc-immo,+flagm",
                        "-filetype=obj", str(src), "-o", str(self.obj)], check=True)

    def test_counts_what_the_target_cpu_lacks(self):
        self.assertEqual(scan(self.obj, have="cortex-a78", want="apple-a13"),
                         {"fjcvtzs": 2, "fcmla": 1, "ldapur": 1, "cfinv": 1})

    def test_reports_nothing_for_the_same_cpu(self):
        self.assertEqual(scan(self.obj, have="apple-a13", want="apple-a13"), {})


if __name__ == "__main__":
    unittest.main()
