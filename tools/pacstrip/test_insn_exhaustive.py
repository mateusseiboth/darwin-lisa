"""Every encoding of the rewritten families, checked against llvm-mc's
disassembler: the rewritten instruction must be the plain form of the
original, with the same registers and offset."""
import re
import shutil
import subprocess
import unittest

from insn import KEEP, PATCH, TRAP, rewrite

LLVM_MC = shutil.which("llvm-mc")


def disassemble(words):
    """One line of assembly per word ('<invalid>' when llvm-mc rejects it)."""
    text = " ".join(f"0x{w & 0xFF:02x} 0x{w >> 8 & 0xFF:02x} 0x{w >> 16 & 0xFF:02x} 0x{w >> 24:02x}" for w in words)
    out = subprocess.run(
        [LLVM_MC, "--disassemble", "--triple=aarch64", "-mattr=+pauth,+jsconv"],
        input=text, capture_output=True, text=True, check=False,
    )
    lines = [l.strip() for l in out.stdout.splitlines() if l.strip() and not l.strip().startswith(".")]
    if len(lines) != len(words):
        raise AssertionError(f"llvm-mc returned {len(lines)} lines for {len(words)} words: {out.stderr[:400]}")
    return [re.sub(r"\s+", " ", l.split("//")[0]).strip() for l in lines]


def is_valid(word):
    """llvm-mc warns on stderr, instead of printing a line, for an unallocated encoding."""
    try:
        disassemble([word])
    except AssertionError:
        return False
    return True


def registers(asm):
    return re.findall(r"\b(?:x\d+|sp|xzr|w\d+)\b", asm)


def offset(asm):
    found = re.search(r"#(-?\d+)", asm)
    return int(found.group(1)) if found else 0


@unittest.skipUnless(LLVM_MC, "llvm-mc is not installed")
class Exhaustive(unittest.TestCase):
    def check_family(self, words, expect):
        originals = disassemble(words)
        results = [rewrite(w) for w in words]
        patched = disassemble([w for _, w in results])
        for word, before, (status, _), after in zip(words, originals, results, patched):
            with self.subTest(word=hex(word), asm=before):
                expect(before, status, after)

    def test_branches(self):
        words = [base | m << 10 | n << 5 | r for base in (0xD71F0800, 0xD73F0800)
                 for m in (0, 1) for n in range(32) for r in range(32)]
        words += [base | m << 10 | n << 5 for base in (0xD61F081F, 0xD63F081F) for m in (0, 1) for n in range(32)]

        def expect(before, status, after):
            self.assertEqual(status, PATCH)
            plain = "blr" if before.startswith("blr") else "br"
            self.assertEqual(after.split()[0], plain)
            self.assertEqual(registers(after)[0], registers(before)[0])

        self.check_family(words, expect)

    def test_signing(self):
        words = [0xDAC10000 | op << 10 | n << 5 | d for op in range(0x12) for n in (0, 7, 30, 31) for d in (0, 16, 30)]
        valid = [w for w in words if is_valid(w)]

        def expect(before, status, after):
            self.assertRegex(before, r"^(pac|aut|xpac)")
            self.assertEqual((status, after), (PATCH, "nop"))

        self.check_family(valid, expect)

    def test_authenticated_loads(self):
        words = [0xF8200400 | m << 23 | (raw >> 9) << 22 | (raw & 0x1FF) << 12 | w << 11 | n << 5 | t
                 for m in (0, 1) for raw in range(1024) for w in (0, 1) for n, t in ((6, 5), (31, 0))]

        def expect(before, status, after):
            if status == TRAP:
                writeback = before.endswith("!")
                in_range = -256 <= offset(before) <= 255 if writeback else offset(before) >= -256
                self.assertFalse(in_range, before)
                return
            self.assertEqual(status, PATCH)
            self.assertRegex(after, r"^ldu?r ")
            self.assertEqual(registers(after), registers(before))
            self.assertEqual(offset(after), offset(before))
            self.assertEqual(after.endswith("!"), before.endswith("!"))

        self.check_family(words, expect)

    def test_pacga(self):
        words = [0x9AC03000 | m << 16 | n << 5 | d for m in (0, 2, 31) for n in (1, 29, 31) for d in (0, 30, 31)]

        def expect(before, status, after):
            self.assertEqual(status, PATCH)
            self.assertTrue(after.startswith("eor "), after)
            self.assertIn("ror #13", after)
            self.assertEqual(registers(after)[:2], registers(before)[:2])

        self.check_family(words, expect)

    def test_hint_space_is_kept(self):
        words = [0xD503233F, 0xD50323BF, 0xD503231F, 0xD503239F, 0xD50320FF]  # paciasp, autiasp, paciaz, autiaz, xpaclri
        for word, asm in zip(words, disassemble(words)):
            with self.subTest(asm=asm):
                self.assertEqual(rewrite(word), (KEEP, word))


if __name__ == "__main__":
    unittest.main()
