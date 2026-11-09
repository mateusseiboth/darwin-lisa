/* SPDX-License-Identifier: MIT */
/* Kernel entry point for EL0 arm64e instruction emulation (see pac_trap.c). */
#ifndef PAC_TRAP_H
#define PAC_TRAP_H

#include <stdbool.h>
#include <stdint.h>
#include <mach/arm/thread_status.h>	/* arm_saved_state_t */

/*
 * Called from handle_uncategorized() in osfmk/arm64/sleh.c for an EL0
 * undefined-instruction trap. Returns true if the instruction was one of the
 * arm64e forms the PAC-stripping pager cannot rewrite (LDRAA/LDRAB/FJCVTZS) and
 * was emulated (the saved state is advanced); false to deliver the exception.
 */
bool pac_emulate_user_trap(arm_saved_state_t *state, uint32_t instr);

#endif /* PAC_TRAP_H */
