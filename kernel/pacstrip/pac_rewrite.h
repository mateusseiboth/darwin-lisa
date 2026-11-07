/* SPDX-License-Identifier: MIT */
/*
 * Rewrite arm64e instructions for a CPU without pointer authentication.
 * The C twin of tools/pacstrip/insn.py, for the kernel's page-in path.
 */
#ifndef PAC_REWRITE_H
#define PAC_REWRITE_H

#include <stddef.h>
#include <stdint.h>

enum pac_rewrite_status {
	PAC_REWRITE_KEEP = 0,	/* not an instruction to change */
	PAC_REWRITE_PATCH = 1,	/* *word now holds the replacement */
	PAC_REWRITE_TRAP = 2,	/* no 4-byte replacement: the kernel emulates it */
};

enum pac_rewrite_status pac_rewrite(uint32_t *word);

/* Rewrite every word of a code buffer in place; returns how many were patched. */
size_t pac_rewrite_buffer(uint32_t *words, size_t count);

#endif
