/* SPDX-License-Identifier: MIT */
/*
 * Emulate LDRAA/LDRAB and FJCVTZS for a CPU without pointer authentication.
 * See pac_emulate.h. No floating point is used: FJCVTZS decodes the IEEE-754
 * double by its integer fields, so this is safe to run in the kernel (which
 * does not save/restore the FPU on its own paths) and identical on the host.
 *
 * The decode masks match tools/pacstrip/insn.py and kernel/pacstrip/pac_rewrite.c:
 *   LDRAA/LDRAB  mask 0xFF200400  value 0xF8200400
 *   FJCVTZS      mask 0xFFFFFC00  value 0x1E7E0000
 */
#include "pac_emulate.h"

#define PAC_NZCV_Z (1u << 30)	/* PSTATE.Z within the NZCV field (bits 31..28 = N,Z,C,V) */

/* LDRAA/LDRAB: base in Xn (or SP for n==31), authenticated (identity here) + scaled offset. */
static enum pac_emulate_result
emulate_ldra(uint32_t insn, struct pac_emulate_ctx *ctx)
{
	unsigned rt = insn & 0x1F;
	unsigned rn = insn >> 5 & 0x1F;
	unsigned writeback = insn >> 11 & 1;
	/* 10-bit signed offset S:imm9, scaled by 8 */
	int32_t raw = (int32_t)(((insn >> 22 & 1) << 9) | (insn >> 12 & 0x1FF));
	int32_t offset = (raw & 0x200 ? raw - 0x400 : raw) * 8;

	uint64_t base = rn == 31 ? ctx->sp : ctx->x[rn];
	uint64_t address = base + (int64_t)offset;

	uint64_t value;
	if (ctx->load64(ctx->mem_ctx, address, &value) != 0) {
		return PAC_EMULATE_FAULT;
	}

	if (rt != 31) {
		ctx->x[rt] = value;
	}
	if (writeback) {
		if (rn == 31) {
			ctx->sp = address;
		} else {
			ctx->x[rn] = address;
		}
	}
	ctx->pc += 4;
	return PAC_EMULATE_OK;
}

/* FJCVTZS: double in Dn -> signed int32 in Wd (round toward zero, modulo 2^32);
 * PSTATE flags become 0:Z:00, Z=1 iff the value was an exact in-range integer. */
static enum pac_emulate_result
emulate_fjcvtzs(uint32_t insn, struct pac_emulate_ctx *ctx)
{
	unsigned rd = insn & 0x1F;
	unsigned rn = insn >> 5 & 0x1F;
	uint64_t bits = ctx->d[rn];

	uint64_t sign = bits >> 63;
	uint32_t exp = (uint32_t)(bits >> 52 & 0x7FF);
	uint64_t mant = bits & 0xFFFFFFFFFFFFFULL;

	uint32_t result;	/* the 32-bit value written to Wd */
	int exact;

	if (exp == 0x7FF) {			/* NaN or infinity */
		result = 0;
		exact = 0;
	} else if (exp == 0 && mant == 0) {	/* +0 or -0 */
		result = 0;
		exact = (sign == 0);		/* -0 reports inexact (safe: the slow path still yields 0) */
	} else {
		uint64_t significand;
		int eff_e;			/* value = significand * 2^eff_e */
		if (exp == 0) {			/* subnormal */
			significand = mant;
			eff_e = -1074;
		} else {			/* normal: implicit leading 1 */
			significand = (1ULL << 52) | mant;
			eff_e = (int)exp - 1023 - 52;
		}

		uint64_t magnitude;		/* trunc(|value|), low 64 bits suffice for the mod-2^32 result */
		int had_frac;
		int in_range;
		if (eff_e >= 0) {
			/* |value| >= 2^52, always out of int32 range; low 32 bits of the shift are the result */
			magnitude = eff_e >= 64 ? 0 : significand << eff_e;
			had_frac = 0;
			in_range = 0;
		} else {
			int shift = -eff_e;
			if (shift >= 64) {
				magnitude = 0;
				had_frac = significand != 0;
			} else {
				magnitude = significand >> shift;
				had_frac = (significand & ((1ULL << shift) - 1)) != 0;
			}
			uint64_t limit = sign ? 0x80000000ULL : 0x7FFFFFFFULL;
			in_range = magnitude <= limit;
		}

		uint32_t low = (uint32_t)magnitude;
		result = sign ? (uint32_t)(-(int32_t)low) : low;	/* two's complement, modulo 2^32 */
		exact = !had_frac && in_range;
	}

	if (rd != 31) {
		ctx->x[rd] = result;		/* writing Wd zero-extends into Xd */
	}
	ctx->nzcv = exact ? PAC_NZCV_Z : 0;	/* FJCVTZS sets NZCV to 0:Z:00, so N=C=V=0 */
	ctx->pc += 4;
	return PAC_EMULATE_OK;
}

enum pac_emulate_result
pac_emulate(uint32_t insn, struct pac_emulate_ctx *ctx)
{
	if ((insn & 0xFF200400u) == 0xF8200400u) {
		return emulate_ldra(insn, ctx);
	}
	if ((insn & 0xFFFFFC00u) == 0x1E7E0000u) {
		return emulate_fjcvtzs(insn, ctx);
	}
	return PAC_EMULATE_UNHANDLED;
}
