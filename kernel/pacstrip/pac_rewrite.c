// SPDX-License-Identifier: MIT
/*
 * Rewrite arm64e instructions for a CPU without pointer authentication.
 *
 * Pointers are never signed on such a CPU, so signing and authenticating are
 * the identity: PAC*, AUT* and XPAC* become NOP, the authenticated branches
 * and loads become their plain forms, and PACGA becomes a deterministic EOR.
 * What has no 4-byte replacement is left for the kernel to emulate.
 *
 * Same table, same results as tools/pacstrip/insn.py; test_pac_rewrite.c
 * checks this file against that one's vectors.
 */
#include "pac_rewrite.h"

#define NOP		0xD503201Fu
#define RET		0xD65F03C0u
#define ERET		0xD69F03E0u
#define BR		0xD61F0000u
#define BLR		0xD63F0000u
#define LDR_UNSIGNED	0xF9400000u
#define LDUR		0xF8400000u
#define LDR_PRE_INDEX	0xF8400C00u
#define EOR_ROR_13	(0xCAC00000u | 13u << 10)

#define PAC_DP1_LAST_OPCODE	0x11u	/* XPACD */
#define PAC_DP1_ZERO_FORMS	0x08u	/* from PACIZA on, Rn must be 31 */

typedef enum pac_rewrite_status (*pac_handler)(uint32_t *word);

static uint32_t rn(uint32_t word)
{
	return word >> 5 & 0x1F;
}

static uint32_t rt(uint32_t word)
{
	return word & 0x1F;
}

static enum pac_rewrite_status patch(uint32_t *word, uint32_t replacement)
{
	*word = replacement;
	return PAC_REWRITE_PATCH;
}

static enum pac_rewrite_status trap(uint32_t *word)
{
	(void)word;
	return PAC_REWRITE_TRAP;
}

static enum pac_rewrite_status to_ret(uint32_t *word)
{
	return patch(word, RET);
}

static enum pac_rewrite_status to_eret(uint32_t *word)
{
	return patch(word, ERET);
}

static enum pac_rewrite_status to_br(uint32_t *word)
{
	return patch(word, BR | rn(*word) << 5);
}

static enum pac_rewrite_status to_blr(uint32_t *word)
{
	return patch(word, BLR | rn(*word) << 5);
}

static enum pac_rewrite_status pac_data_processing(uint32_t *word)
{
	uint32_t opcode = *word >> 10 & 0x3F;

	if (opcode > PAC_DP1_LAST_OPCODE)
		return PAC_REWRITE_KEEP;
	if (opcode >= PAC_DP1_ZERO_FORMS && rn(*word) != 31)
		return PAC_REWRITE_KEEP;
	return patch(word, NOP);
}

static enum pac_rewrite_status pacga_to_eor(uint32_t *word)
{
	return patch(word, EOR_ROR_13 | (*word & 0x001F03FFu));
}

static int32_t ldra_offset(uint32_t word)
{
	int32_t raw = (int32_t)((word >> 22 & 1) << 9 | (word >> 12 & 0x1FF));

	return (raw & 0x200 ? raw - 0x400 : raw) * 8;
}

static enum pac_rewrite_status ldra_to_ldr(uint32_t *word)
{
	int32_t offset = ldra_offset(*word);
	uint32_t registers = rn(*word) << 5 | rt(*word);
	uint32_t imm9 = (uint32_t)offset & 0x1FF;

	if (*word >> 11 & 1) {
		if (offset < -256 || offset > 255)
			return PAC_REWRITE_TRAP;
		return patch(word, LDR_PRE_INDEX | imm9 << 12 | registers);
	}
	if (offset >= 0)
		return patch(word, LDR_UNSIGNED | (uint32_t)(offset >> 3) << 10 | registers);
	if (offset >= -256)
		return patch(word, LDUR | imm9 << 12 | registers);
	return PAC_REWRITE_TRAP;
}

static const struct {
	uint32_t mask;
	uint32_t value;
	pac_handler handler;
} strategies[] = {
	{ 0xFFFFFBFFu, 0xD65F0BFFu, to_ret },			/* RETAA, RETAB */
	{ 0xFFFFFBFFu, 0xD69F0BFFu, to_eret },			/* ERETAA, ERETAB */
	{ 0xFFFFF800u, 0xD71F0800u, to_br },			/* BRAA, BRAB */
	{ 0xFFFFF81Fu, 0xD61F081Fu, to_br },			/* BRAAZ, BRABZ */
	{ 0xFFFFF800u, 0xD73F0800u, to_blr },			/* BLRAA, BLRAB */
	{ 0xFFFFF81Fu, 0xD63F081Fu, to_blr },			/* BLRAAZ, BLRABZ */
	{ 0xFFFF0000u, 0xDAC10000u, pac_data_processing },	/* PAC*, AUT*, XPAC* */
	{ 0xFFE0FC00u, 0x9AC03000u, pacga_to_eor },		/* PACGA */
	{ 0xFF200400u, 0xF8200400u, ldra_to_ldr },		/* LDRAA, LDRAB */
	{ 0xFFFFFC00u, 0x1E7E0000u, trap },			/* FJCVTZS (ARMv8.3 JSCVT) */
};

enum pac_rewrite_status pac_rewrite(uint32_t *word)
{
	size_t i;

	for (i = 0; i < sizeof(strategies) / sizeof(strategies[0]); i++)
		if ((*word & strategies[i].mask) == strategies[i].value)
			return strategies[i].handler(word);
	return PAC_REWRITE_KEEP;
}

size_t pac_rewrite_buffer(uint32_t *words, size_t count)
{
	size_t i, patched = 0;

	for (i = 0; i < count; i++)
		patched += pac_rewrite(&words[i]) == PAC_REWRITE_PATCH;
	return patched;
}
