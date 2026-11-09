/* SPDX-License-Identifier: MIT */
/*
 * Return-to-loader debug channel for a UniXNU kernel booted over the HakiOS
 * xnuloader. Adapted from HakiOS's osfmk/arm64/loader_return.c, stripped of the
 * HakiOS driver/GIC/PSCI quiesce that a bring-up kernel does not have.
 *
 * On hardware with no reachable serial, the loader keeps UEFI intact and
 * publishes the physical address of a return stub in loader_stub_slot (it scans
 * the kernel image for the "XNULDRST" magic). When the kernel panics, or when
 * loader_return_after seconds pass, the kernel collects its log, maps the stub
 * 1:1 through TTBR0 and jumps to it; the loader writes the log out over USB.
 * A watching loader can also read loader_live (magic "XNULIVE!") while it runs.
 *
 * See loader_console.h. Hooked from PE_init_kprintf / PE_init_printf (console
 * tee) and from panic_spin_forever / kdp_machine_reboot_type in kern/debug.c.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <machine/atomic.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>

#include "loader_console.h"

#define LOADER_LOG_SIZE (1536 * 1024)
#define LOADER_LOG_HEAD (256 * 1024)
#define LOADER_LOG_RING (1024 * 1024)

/* Also the target of the console tee; char buffer the loader reads. */
char     loader_log[LOADER_LOG_SIZE];
uint32_t loader_log_len;
static bool loader_return_entered;

/* One L1 and one L2 table map the single 2 MB block that holds the stub. */
static uint64_t loader_l1[512] __attribute__((aligned(4096)));
static uint64_t loader_l2[512] __attribute__((aligned(4096)));

static uint32_t loader_log_ring_at;
static bool     loader_log_wrapped;
static bool     loader_log_linear;
static char     loader_log_scratch[LOADER_LOG_RING];
static uint32_t loader_log_lock;

static void
loader_log_put_locked(char c)
{
	if (loader_log_linear || loader_log_len < LOADER_LOG_HEAD) {
		if (loader_log_len < LOADER_LOG_SIZE) {
			loader_log[loader_log_len++] = c;
		}
	} else {
		loader_log[LOADER_LOG_HEAD + loader_log_ring_at] = c;
		if (++loader_log_ring_at == LOADER_LOG_RING) {
			loader_log_ring_at = 0;
			loader_log_wrapped = true;
		}
		if (!loader_log_wrapped) {
			loader_log_len = LOADER_LOG_HEAD + loader_log_ring_at;
		}
	}
}

void
loader_log_putc(char c)
{
	uint64_t daif;
	bool locked = false;

	if (c == '\r') {
		return;
	}
	daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
		locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	loader_log_put_locked(c);
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
}

static void
loader_log_puts(const char *s)
{
	for (; *s; s++) {
		loader_log_putc(*s);
	}
}

/* Oldest first: the head, a note about what was dropped, then the ring from its oldest char. */
static void
loader_log_linearize(void)
{
	static const char note[] = "\n\n[... the log between the first 256 KB and the last 1 MB was dropped ...]\n\n";
	uint32_t older = LOADER_LOG_RING - loader_log_ring_at, n = LOADER_LOG_HEAD;

	if (loader_log_linear) {
		return;
	}
	loader_log_linear = true;
	if (!loader_log_wrapped) {
		return;
	}
	memcpy(loader_log_scratch, loader_log + LOADER_LOG_HEAD + loader_log_ring_at, older);
	memcpy(loader_log_scratch + older, loader_log + LOADER_LOG_HEAD, loader_log_ring_at);
	memcpy(loader_log + n, note, sizeof(note) - 1);
	n += sizeof(note) - 1;
	memcpy(loader_log + n, loader_log_scratch + sizeof(note) - 1, LOADER_LOG_RING - (sizeof(note) - 1));
	loader_log_len = LOADER_LOG_HEAD + LOADER_LOG_RING;
}

/* Patched by the loader: { "XNULDRST", PA of its return stub, checkpoint, PA - VA }. */
uint64_t loader_stub_slot[4] __attribute__((used, aligned(8))) = { 0x545352444C554E58ULL, 0, 0, 0 };

/* The loader reads these (magic "XNULIVE!") while the kernel runs. */
static uint64_t loader_recording[3];
static uint64_t loader_sound[3];

struct loader_live {
	uint64_t magic;
	uint64_t heartbeat;
	const char *log;
	const uint32_t *log_len;
	const uint32_t *log_ring_at;
	const bool *log_wrapped;
	uint64_t log_head, log_ring;
	const uint64_t *recording;
	volatile uint64_t stop;
	uint64_t magic2;
	const uint64_t *sound;
};

struct loader_live loader_live __attribute__((used)) = {
	.magic = 0x214556494C554E58ULL, .log = loader_log, .log_len = &loader_log_len,
	.log_ring_at = &loader_log_ring_at, .log_wrapped = &loader_log_wrapped,
	.log_head = LOADER_LOG_HEAD, .log_ring = LOADER_LOG_RING, .recording = loader_recording,
	.magic2 = 0x324556494C554E58ULL, .sound = loader_sound,
};

/* Valid from the first C instruction: does not need gVirtBase/gPhysBase. */
static inline uint64_t
static_kvtophys(const void *va)
{
	return (uint64_t)va + loader_stub_slot[3];
}

/* Jump to the loader's return stub. reason: 0 = orderly end, 2 = checkpoint. */
static void
loader_jump(uint64_t reason, uint64_t code)
{
#if ARM_PGSHIFT == 12
	uint64_t stub = loader_stub_slot[1];
	uint64_t tcr, t0sz;

	if (stub == 0 || loader_return_entered) {
		return;
	}
	tcr = __builtin_arm_rsr64("TCR_EL1");
	t0sz = tcr & 0x3f;
	if (t0sz < 25 || t0sz > 33 || (stub >> (64 - t0sz)) != 0) {
		return;
	}
	loader_return_entered = true;

	loader_l1[stub >> 30] = static_kvtophys(loader_l2) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
	loader_l2[(stub >> 21) & 0x1ff] = (stub & ~0x1fffffULL) | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |
	    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_AP(AP_RWNA) |
	    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);

	/* The stub turns the MMU off; flush the log back to RAM so it survives. */
	for (uintptr_t p = (uintptr_t)loader_log & ~63UL; p < (uintptr_t)loader_log + loader_log_len; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");

	register uint64_t r_ttbr0 __asm__("x0") = static_kvtophys(loader_l1);
	register uint64_t r_log __asm__("x2") = static_kvtophys(loader_log);
	register uint64_t r_len __asm__("x3") = loader_log_len;
	register uint64_t r_tcr __asm__("x4") = tcr;
	register uint64_t r_stub __asm__("x5") = stub;
	register uint64_t r_reason __asm__("x6") = reason;
	register uint64_t r_code __asm__("x7") = code;

	__asm__ volatile (
		"msr DAIFSet, #0xf\n"
		"bic x4, x4, #0x80\n"		/* TCR_EL1.EPD0: allow TTBR0 walks */
		"msr TCR_EL1, x4\n"
		"msr TTBR0_EL1, x0\n"
		"isb\n"
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb\n"
		"mov x0, x2\n"
		"mov x1, x3\n"
		"mov x2, x6\n"
		"mov x3, x7\n"
		"br x5\n"
		: : "r" (r_ttbr0), "r" (r_log), "r" (r_len), "r" (r_tcr), "r" (r_stub),
		    "r" (r_reason), "r" (r_code));
	__builtin_unreachable();
#else
	(void)reason; (void)code;
#endif
}

extern char *debug_buf_base, *debug_buf_ptr;

/* bsd/sys/msgbuf.h (not includable from osfmk) */
struct msgbuf {
	int   msg_magic;
	int   msg_size;
	int   msg_bufx;
	int   msg_bufr;
	char *msg_bufc;
};
extern struct msgbuf *msgbufp;

/* Everything printf()/IOLog()/os_log wrote ends up in the kernel message buffer. */
static void
loader_log_append_msgbuf(void)
{
	struct msgbuf *mb = msgbufp;

	if (mb == NULL || mb->msg_bufc == NULL || mb->msg_size <= 0 || mb->msg_bufx < 0 || mb->msg_bufx > mb->msg_size) {
		return;
	}
	loader_log_puts("\n--- kernel message buffer ---\n");
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? mb->msg_bufx : 0;
		int to = pass == 0 ? mb->msg_size : mb->msg_bufx;
		for (int i = from; i < to; i++) {
			if (mb->msg_bufc[i] != 0) {
				loader_log_putc(mb->msg_bufc[i]);
			}
		}
	}
}

void
loader_return(void)
{
	loader_log_linearize();
	if (!loader_return_entered) {
		loader_log_append_msgbuf();
	}
	if (!loader_return_entered && debug_buf_base != NULL && debug_buf_ptr > debug_buf_base &&
	    (size_t)(debug_buf_ptr - debug_buf_base) < 256 * 1024) {
		loader_log_puts("\n--- panic buffer ---\n");
		for (const char *c = debug_buf_base; c < debug_buf_ptr; c++) {
			loader_log_putc(*c);
		}
	}
	loader_jump(0, 0);
}

/* boot-arg loader_return_after=<seconds>: hand control back after that long. */
static void
loader_return_timer(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	loader_log_puts("\n--- loader_return_after expired ---\n");
	loader_return();
}

__startup_func
static void
loader_return_timer_init(void)
{
	uint32_t seconds = 0;
	uint64_t deadline;
	thread_call_t call;

	if (!PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	call = thread_call_allocate(loader_return_timer, NULL);
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_return_timer_init);

/* boot-arg hv_wdt_pet=1: feed the Qualcomm hypervisor watchdog (else it resets the phone). */
#define VIRT_WDT_PET       0x86000007u
#define HV_WDT_PET_SECONDS 10

static thread_call_t hv_wdt_call;
static uint32_t hv_wdt_enabled;

static void
hv_wdt_pet(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (hv_wdt_enabled) {
		register uint64_t x0 __asm__("x0") = VIRT_WDT_PET;
		register uint64_t x1 __asm__("x1") = 0;
		register uint64_t x2 __asm__("x2") = 0;
		register uint64_t x3 __asm__("x3") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	loader_live.heartbeat++;
	clock_interval_to_deadline(HV_WDT_PET_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(hv_wdt_call, deadline);
}

__startup_func
static void
hv_wdt_pet_init(void)
{
	PE_parse_boot_argn("hv_wdt_pet", &hv_wdt_enabled, sizeof(hv_wdt_enabled));
	hv_wdt_call = thread_call_allocate(hv_wdt_pet, NULL);
	hv_wdt_pet(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hv_wdt_pet_init);

/* ---- console tees (installed from PE_init_kprintf / PE_init_printf) ---- */

typedef void (*lc_putc_t)(char);
static lc_putc_t lc_orig_kputc;
static lc_putc_t lc_orig_putc;

static void
lc_tee_kputc(char c)
{
	loader_log_putc(c);
	if (lc_orig_kputc != 0) {
		lc_orig_kputc(c);
	}
}

static void
lc_tee_putc(char c)
{
	loader_log_putc(c);
	if (lc_orig_putc != 0) {
		lc_orig_putc(c);
	}
}

void
loader_console_tap_kputc(void)
{
	if (PE_kputc == lc_tee_kputc) {
		return;
	}
	lc_orig_kputc = PE_kputc;
	PE_kputc = lc_tee_kputc;
}

void
loader_console_tap_putc(void)
{
	if (PE_putc == lc_tee_putc) {
		return;
	}
	lc_orig_putc = PE_putc;
	PE_putc = lc_tee_putc;
}
