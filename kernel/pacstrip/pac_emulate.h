/* SPDX-License-Identifier: MIT */
/*
 * Emulate the arm64e instructions that have no 4-byte replacement, for a CPU
 * without pointer authentication. These are the traps pac_rewrite()/insn.py
 * leaves behind (PAC_REWRITE_TRAP): the undefined-instruction handler calls
 * pac_emulate() for them. Phase 1's scan of the real iOS 26.5.2 cache found
 * exactly three kinds across the whole shared cache:
 *
 *   LDRAA / LDRAB  authenticated load, offset outside the in-place LDUR/LDR
 *                  window (W=0 & offset < -256, or W=1 & offset not in
 *                  [-256,255]); the pager rewrites the rest.
 *   FJCVTZS        FEAT_JSCVT double -> int32 with JavaScript rounding; the
 *                  A78 lacks the feature.
 *
 * In the rewritten world no pointer is ever signed (every PAC* became a NOP),
 * so authentication is the identity: LDRAA/LDRAB are a plain base+offset load.
 *
 * The core is pure and host-testable (test_pac_emulate.c): it touches no XNU
 * types and reads memory only through a caller-supplied callback, so the kernel
 * can route the load through a faulting-safe path and the test through a buffer.
 */
#ifndef PAC_EMULATE_H
#define PAC_EMULATE_H

#include <stdbool.h>
#include <stdint.h>

enum pac_emulate_result {
	PAC_EMULATE_UNHANDLED = 0,	/* not one of the instructions we emulate; leave the trap alone */
	PAC_EMULATE_OK = 1,		/* emulated; PC already advanced past it */
	PAC_EMULATE_FAULT = 2,		/* the memory access faulted; deliver the original fault */
};

struct pac_emulate_ctx {
	uint64_t x[31];		/* X0..X30 */
	uint64_t sp;		/* current stack pointer (Rn==31 means SP for LDRAA/LDRAB) */
	uint64_t pc;		/* the trapping instruction's address; advanced by 4 on success */
	uint64_t d[32];		/* V0..V31, low 64 bits: the double source for FJCVTZS */
	uint32_t nzcv;		/* PSTATE flags in bits 31..28 (N,Z,C,V); FJCVTZS rewrites them */

	/* Read 8 bytes at addr. Returns 0 on success (value in *out), non-zero to fault. */
	int (*load64)(void *mem_ctx, uint64_t addr, uint64_t *out);
	void *mem_ctx;
};

/* Emulate one instruction. On PAC_EMULATE_OK the context (registers, flags, pc) is updated. */
enum pac_emulate_result pac_emulate(uint32_t insn, struct pac_emulate_ctx *ctx);

#endif /* PAC_EMULATE_H */
