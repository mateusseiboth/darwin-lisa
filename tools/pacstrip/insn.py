"""Rewrite one arm64e instruction for a CPU without pointer authentication.

Pointers are never signed on such a CPU, so signing and authenticating are
the identity: PAC*/AUT*/XPAC* become NOP, the authenticated branches and
loads become their plain forms, and PACGA becomes a deterministic EOR (its
result is only ever compared with another PACGA of the same inputs).

rewrite(word) returns (KEEP, word), (PATCH, new_word) or (TRAP, word) for an
instruction that has no 4-byte replacement and must be emulated by the
kernel. The hint-space forms (PACIASP, AUTIBSP, ...) already execute as NOP
and are kept.

Encodings: Arm ARM, checked with llvm-mc -mattr=+pauth.
"""

KEEP, PATCH, TRAP = "keep", "patch", "trap"

NOP = 0xD503201F
RET = 0xD65F03C0
ERET = 0xD69F03E0
BR = 0xD61F0000
BLR = 0xD63F0000
LDR_UNSIGNED = 0xF9400000
LDUR = 0xF8400000
LDR_PRE_INDEX = 0xF8400C00
EOR_ROR_13 = 0xCAC00000 | 13 << 10

PAC_DP1_LAST_OPCODE = 0x11  # XPACD
PAC_DP1_ZERO_FORMS = 0x08  # from PACIZA on, Rn must be 31


def rn(word):
    return word >> 5 & 0x1F


def rt(word):
    return word & 0x1F


def keep(word):
    return KEEP, word


def trap(word):
    return TRAP, word


def to_ret(_word):
    return PATCH, RET


def to_eret(_word):
    return PATCH, ERET


def to_br(word):
    return PATCH, BR | rn(word) << 5


def to_blr(word):
    return PATCH, BLR | rn(word) << 5


def pac_data_processing(word):
    opcode = word >> 10 & 0x3F
    if opcode > PAC_DP1_LAST_OPCODE:
        return keep(word)
    if opcode >= PAC_DP1_ZERO_FORMS and rn(word) != 31:
        return keep(word)
    return PATCH, NOP


def pacga_to_eor(word):
    return PATCH, EOR_ROR_13 | (word & 0x001F03FF)


def ldra_offset(word):
    raw = (word >> 22 & 1) << 9 | (word >> 12 & 0x1FF)
    signed = raw - (1 << 10) if raw & (1 << 9) else raw
    return signed * 8


def ldr_without_writeback(word, offset, registers):
    if offset >= 0:
        return PATCH, LDR_UNSIGNED | (offset >> 3) << 10 | registers
    if offset >= -256:
        return PATCH, LDUR | (offset & 0x1FF) << 12 | registers
    return trap(word)


def ldr_with_writeback(word, offset, registers):
    if -256 <= offset <= 255:
        return PATCH, LDR_PRE_INDEX | (offset & 0x1FF) << 12 | registers
    return trap(word)


def ldra_to_ldr(word):
    registers = rn(word) << 5 | rt(word)
    writeback = word >> 11 & 1
    form = ldr_with_writeback if writeback else ldr_without_writeback
    return form(word, ldra_offset(word), registers)


# (mask, value, handler): the first match wins
STRATEGIES = (
    (0xFFFFFBFF, 0xD65F0BFF, to_ret),  # RETAA, RETAB
    (0xFFFFFBFF, 0xD69F0BFF, to_eret),  # ERETAA, ERETAB
    (0xFFFFF800, 0xD71F0800, to_br),  # BRAA, BRAB
    (0xFFFFF81F, 0xD61F081F, to_br),  # BRAAZ, BRABZ
    (0xFFFFF800, 0xD73F0800, to_blr),  # BLRAA, BLRAB
    (0xFFFFF81F, 0xD63F081F, to_blr),  # BLRAAZ, BLRABZ
    (0xFFFF0000, 0xDAC10000, pac_data_processing),  # PAC*, AUT*, XPAC*
    (0xFFE0FC00, 0x9AC03000, pacga_to_eor),  # PACGA
    (0xFF200400, 0xF8200400, ldra_to_ldr),  # LDRAA, LDRAB
    (0xFFFFFC00, 0x1E7E0000, trap),  # FJCVTZS (ARMv8.3 JSCVT)
)


def rewrite(word):
    for mask, value, handler in STRATEGIES:
        if word & mask == value:
            return handler(word)
    return keep(word)
