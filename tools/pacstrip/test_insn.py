"""Instruction rewriting: every encoding below was checked with llvm-mc
(--triple=aarch64 -mattr=+pauth -show-encoding)."""
import unittest

from insn import KEEP, PATCH, TRAP, rewrite

NOP = 0xD503201F


def le(*b):
    return int.from_bytes(bytes(b), "little")


class ReturnsAndBranches(unittest.TestCase):
    def test_retab_becomes_ret(self):
        self.assertEqual(rewrite(le(0xFF, 0x0F, 0x5F, 0xD6)), (PATCH, le(0xC0, 0x03, 0x5F, 0xD6)))

    def test_retaa_becomes_ret(self):
        self.assertEqual(rewrite(le(0xFF, 0x0B, 0x5F, 0xD6)), (PATCH, le(0xC0, 0x03, 0x5F, 0xD6)))

    def test_braa_becomes_br_of_the_same_register(self):
        # braa x16, x17 -> br x16
        self.assertEqual(rewrite(le(0x11, 0x0A, 0x1F, 0xD7)), (PATCH, le(0x00, 0x02, 0x1F, 0xD6)))

    def test_braaz_becomes_br(self):
        # braaz x3 -> br x3
        self.assertEqual(rewrite(le(0x7F, 0x08, 0x1F, 0xD6)), (PATCH, 0xD61F0060))

    def test_blraaz_becomes_blr(self):
        # blraaz x8 -> blr x8
        self.assertEqual(rewrite(le(0x1F, 0x09, 0x3F, 0xD6)), (PATCH, le(0x00, 0x01, 0x3F, 0xD6)))

    def test_blrab_with_sp_modifier_becomes_blr(self):
        # blrab x4, sp -> blr x4
        self.assertEqual(rewrite(le(0x9F, 0x0C, 0x3F, 0xD7)), (PATCH, 0xD63F0080))

    def test_eretab_becomes_eret(self):
        self.assertEqual(rewrite(le(0xFF, 0x0F, 0x9F, 0xD6)), (PATCH, le(0xE0, 0x03, 0x9F, 0xD6)))


class PointerSigning(unittest.TestCase):
    def test_sign_and_authenticate_become_nop(self):
        for word in (
            le(0x30, 0x02, 0xC1, 0xDA),  # pacia x16, x17
            le(0x41, 0x04, 0xC1, 0xDA),  # pacib x1, x2
            le(0xE7, 0x2F, 0xC1, 0xDA),  # pacdzb x7
            le(0x20, 0x18, 0xC1, 0xDA),  # autda x0, x1
            le(0x20, 0x1C, 0xC1, 0xDA),  # autdb x0, x1
            le(0xFE, 0x33, 0xC1, 0xDA),  # autiza x30
            le(0xE3, 0x43, 0xC1, 0xDA),  # xpaci x3
            le(0xE9, 0x47, 0xC1, 0xDA),  # xpacd x9
        ):
            with self.subTest(word=hex(word)):
                self.assertEqual(rewrite(word), (PATCH, NOP))

    def test_pacga_becomes_a_deterministic_eor(self):
        # pacga x0, x1, x2 -> eor x0, x1, x2, ror #13
        self.assertEqual(rewrite(le(0x20, 0x30, 0xC2, 0x9A)), (PATCH, le(0x20, 0x34, 0xC2, 0xCA)))

    def test_pacga_keeps_its_registers(self):
        # pacga x30, x29, sp: Rm 31 reads as xzr in the eor, still a function of the inputs
        self.assertEqual(rewrite(le(0xBE, 0x33, 0xDF, 0x9A)), (PATCH, 0xCADF37BE))

    def test_hint_space_forms_are_left_alone(self):
        # paciasp is a NOP on a CPU without pointer authentication
        self.assertEqual(rewrite(le(0x3F, 0x23, 0x03, 0xD5)), (KEEP, le(0x3F, 0x23, 0x03, 0xD5)))


class AuthenticatedLoads(unittest.TestCase):
    def test_negative_offset_becomes_ldur(self):
        # ldraa x9, [x0, #-8] -> ldur x9, [x0, #-8]
        self.assertEqual(rewrite(le(0x09, 0xF4, 0x7F, 0xF8)), (PATCH, le(0x09, 0x80, 0x5F, 0xF8)))

    def test_positive_offset_becomes_ldr_unsigned(self):
        # ldraa x5, [x6, #4088] -> ldr x5, [x6, #4088]
        self.assertEqual(rewrite(le(0xC5, 0xF4, 0x3F, 0xF8)), (PATCH, le(0xC5, 0xFC, 0x47, 0xF9)))

    def test_writeback_in_range_becomes_pre_index(self):
        # ldraa x5, [x6, #-256]! -> ldr x5, [x6, #-256]!
        self.assertEqual(rewrite(le(0xC5, 0x0C, 0x7E, 0xF8)), (PATCH, le(0xC5, 0x0C, 0x50, 0xF8)))

    def test_offset_out_of_ldur_range_traps(self):
        for word in (
            le(0xC5, 0x04, 0x60, 0xF8),  # ldraa x5, [x6, #-4096]
            le(0xC5, 0xF4, 0xFD, 0xF8),  # ldrab x5, [x6, #-264]
        ):
            with self.subTest(word=hex(word)):
                self.assertEqual(rewrite(word), (TRAP, word))

    def test_writeback_out_of_range_traps(self):
        word = le(0x41, 0xFC, 0xBF, 0xF8)  # ldrab x1, [x2, #4088]!
        self.assertEqual(rewrite(word), (TRAP, word))


class OtherInstructions(unittest.TestCase):
    def test_ordinary_code_is_kept(self):
        for word in (le(0xC0, 0x03, 0x5F, 0xD6), NOP, le(0x20, 0x34, 0xC2, 0xCA)):
            with self.subTest(word=hex(word)):
                self.assertEqual(rewrite(word), (KEEP, word))

    def test_fjcvtzs_is_reported_for_the_kernel_to_emulate(self):
        word = le(0x20, 0x00, 0x7E, 0x1E)  # fjcvtzs w0, d1 (ARMv8.3 JSCVT)
        self.assertEqual(rewrite(word), (TRAP, word))


if __name__ == "__main__":
    unittest.main()
