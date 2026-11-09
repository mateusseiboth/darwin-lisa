/* SPDX-License-Identifier: MIT */
/* See loader_console.h. Compiled into pexpert; hooked from PE_init_kprintf and PE_init_printf. */
#include <stdint.h>
#include <pexpert/pexpert.h>		/* PE_putc */
#include <mach/vm_param.h>		/* vm_kernel_slide */

#include "loader_console.h"

/* Must match struct kernel_live in the xnuloader (loader.c). All uint64. */
struct kernel_live {
	uint64_t magic;			/* "XNULIVE!" */
	uint64_t heartbeat;
	uint64_t log, log_len, log_ring_at, log_wrapped;
	uint64_t log_head, log_ring;
	uint64_t recording;
	uint64_t stop;
	uint64_t magic2;		/* "XNULIVE2" */
	uint64_t sound;
};

#define LOADER_LIVE_MAGIC  0x214556494C554E58ULL	/* "XNULIVE!" */
#define LOADER_LIVE_MAGIC2 0x324556494C554E58ULL	/* "XNULIVE2" */

#define LC_BUFSIZE (256u * 1024u)

static char lc_buf[LC_BUFSIZE] __attribute__((aligned(64)));
static volatile uint32_t lc_len;			/* loader reads *(uint32*)(live.log_len + slide) */

/* The struct the loader scans for. Magic fields set at compile time so a very early
 * scan still finds it; the addresses are filled in once vm_kernel_slide is known. */
struct kernel_live loader_live __attribute__((used, aligned(8))) = {
	.magic = LOADER_LIVE_MAGIC,
	.magic2 = LOADER_LIVE_MAGIC2,
};

static void
loader_console_arm(void)
{
	if (loader_live.log != 0) {
		return;
	}
	/* The loader adds the kernel slide to these, so report the unslid VAs. */
	loader_live.log = (uint64_t)(uintptr_t)lc_buf - (uint64_t)vm_kernel_slide;
	loader_live.log_len = (uint64_t)(uintptr_t)&lc_len - (uint64_t)vm_kernel_slide;
	loader_live.log_head = LC_BUFSIZE;	/* a flat buffer: no ring */
	loader_live.log_ring = 0;
	loader_live.heartbeat = 1;
}

static void
lc_putc(char c)
{
	uint32_t n = lc_len;
	if (n < LC_BUFSIZE) {
		lc_buf[n] = c;
		lc_len = n + 1;		/* publish the new length for the loader's poll */
	}
}

typedef void (*lc_putc_t)(char);
static lc_putc_t lc_orig_kputc;
static lc_putc_t lc_orig_putc;

static void
lc_tee_kputc(char c)
{
	lc_putc(c);
	if (lc_orig_kputc != 0) {
		lc_orig_kputc(c);
	}
}

static void
lc_tee_putc(char c)
{
	lc_putc(c);
	if (lc_orig_putc != 0) {
		lc_orig_putc(c);
	}
}

void
loader_console_tap_kputc(void)
{
	loader_console_arm();
	if (PE_kputc == lc_tee_kputc) {
		return;
	}
	lc_orig_kputc = PE_kputc;
	PE_kputc = lc_tee_kputc;
}

void
loader_console_tap_putc(void)
{
	loader_console_arm();
	if (PE_putc == lc_tee_putc) {
		return;
	}
	lc_orig_putc = PE_putc;
	PE_putc = lc_tee_putc;
}
