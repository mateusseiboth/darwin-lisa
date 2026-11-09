/* SPDX-License-Identifier: MIT */
/*
 * Kernel glue between XNU's EL0 undefined-instruction handler and pac_emulate().
 *
 * This file is compiled into osfmk and called from handle_uncategorized() in
 * osfmk/arm64/sleh.c (see the darwin-lisa patch series). It marshals the trapped
 * thread's saved register state into a struct pac_emulate_ctx, runs the pure
 * emulator, and writes the result back. The emulator itself (pac_emulate.c) is
 * host-tested; this file only moves registers.
 *
 * Reading Dn is safe here: EL0 synchronous exceptions spill the NEON block to
 * thread->machine.uNeon via SPILL_REGISTERS before C code runs, so uNeon is the
 * current FP state at trap time.
 */
#include <stdint.h>
#include <stdbool.h>
#include <kern/thread.h>
#include <arm64/proc_reg.h>
#include <mach/arm/thread_status.h>
#include <vm/vm_map_xnu.h>	/* copyin() */

#include "pac_emulate.h"
#include "pac_trap.h"

static int
pac_trap_load64(void *mem_ctx, uint64_t addr, uint64_t *out)
{
	uint64_t value;
	(void)mem_ctx;
	if (copyin((user_addr_t)addr, &value, sizeof(value)) != 0) {
		return -1;
	}
	*out = value;
	return 0;
}

/*
 * Try to emulate a trapping arm64e instruction for a 64-bit user thread.
 * Returns true if it was emulated (state advanced); false to let XNU deliver
 * the original EXC_BAD_INSTRUCTION.
 */
bool
pac_emulate_user_trap(arm_saved_state_t *state, uint32_t instr)
{
	struct pac_emulate_ctx ctx;
	enum pac_emulate_result result;
	unsigned i;

	if (PSR64_IS_KERNEL(get_saved_state_cpsr(state)) || !is_saved_state64(state)) {
		return false;
	}

	for (i = 0; i < 31; i++) {
		ctx.x[i] = get_saved_state_reg(state, i);
	}
	ctx.sp = get_saved_state_sp(state);
	ctx.pc = get_saved_state_pc(state);
	ctx.nzcv = get_saved_state_cpsr(state) & PSR64_NZCV_MASK;

	/* FP source for FJCVTZS; LDRAA/LDRAB never read it. uNeon is NULL only for a
	 * thread that has never used FP, which cannot be executing FJCVTZS. */
	arm_neon_saved_state_t *neon = current_thread()->machine.uNeon;
	if (neon != NULL) {
		const arm_neon_saved_state64_t *ns = neon_state64(neon);
		for (i = 0; i < 32; i++) {
			ctx.d[i] = ns->v.d[i][0];	/* low 64 bits = Dn */
		}
	} else {
		for (i = 0; i < 32; i++) {
			ctx.d[i] = 0;
		}
	}

	ctx.load64 = pac_trap_load64;
	ctx.mem_ctx = NULL;

	result = pac_emulate(instr, &ctx);
	if (result != PAC_EMULATE_OK) {
		return false;
	}

	for (i = 0; i < 31; i++) {
		set_saved_state_reg(state, i, ctx.x[i]);
	}
	set_saved_state_sp(state, ctx.sp);
	set_saved_state_pc(state, ctx.pc);
	set_saved_state_cpsr(state,
	    (get_saved_state_cpsr(state) & ~PSR64_NZCV_MASK) | (ctx.nzcv & PSR64_NZCV_MASK));
	return true;
}
