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
#include <kern/cpu_number.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>

#include "loader_console.h"

/* 256 KiB total is ~14x the largest boot log we capture (~18 KiB); keeping it
 * small matters because this buffer is static __DATA (__common/__bss) that the
 * Phase 3 prelink must fit inside the kernelcache's __DATA slot before the
 * retained kexts. The old 1.5 MB ring overflowed that slot by ~1.75 MB. */
/* darwin-lisa (display work): the Phase 3 prelink is not the boot path any more (Caminho B), and a run to the window
 * server writes megabytes (launchd logs every service to the console in the restore environment): 1.5 MB again, the
 * size the loader has always taken. */
#define LOADER_LOG_SIZE (1536 * 1024)
#define LOADER_LOG_HEAD (256 * 1024)
#define LOADER_LOG_RING (1280 * 1024)

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

/*
 * darwin-lisa: in the restore environment launchd writes every state change of every service to the console
 * ("com.apple.xpc.launchd|... <Notice>: service state: spawned"), megabytes of it, and the log the loader hands out
 * is then all launchd and none of the kernel's later lines. Those lines are held back here, a line at a time:
 * a launchd <Notice> line goes to the log only when it says something exited or crashed; everything else as it was.
 */
static char     loader_line[512];
static uint32_t loader_line_len;
static const char loader_launchd[] = "com.apple.xpc.launchd|";

static bool
loader_line_has(const char *what)
{
	size_t n = strlen(what);

	for (uint32_t i = 0; i + n <= loader_line_len; i++) {
		if (memcmp(loader_line + i, what, n) == 0) {
			return true;
		}
	}
	return false;
}

/* with the lock held */
static void
loader_line_putc_locked(char c)
{
	bool launchd = loader_line_len >= sizeof(loader_launchd) - 1 &&
	    memcmp(loader_line, loader_launchd, sizeof(loader_launchd) - 1) == 0;

	if (loader_line_len < sizeof(loader_launchd) - 1) {
		/* not yet known whose the line is: hold it while it may still be launchd's */
		if (c == loader_launchd[loader_line_len]) {
			loader_line[loader_line_len++] = c;
			return;
		}
		for (uint32_t i = 0; i < loader_line_len; i++) {
			loader_log_put_locked(loader_line[i]);
		}
		loader_line_len = 0;
		loader_log_put_locked(c);
		return;
	}
	if (!launchd) {
		loader_log_put_locked(c);
		return;
	}
	if (c != '\n' && loader_line_len < sizeof(loader_line)) {
		loader_line[loader_line_len++] = c;
		return;
	}
	if (!loader_line_has("<Notice>") || loader_line_has("exited") || loader_line_has("crash") ||
	    loader_line_has("darwinlisa")) {
		for (uint32_t i = 0; i < loader_line_len; i++) {
			loader_log_put_locked(loader_line[i]);
		}
		loader_log_put_locked('\n');
	}
	loader_line_len = 0;
	if (c != '\n') {
		loader_log_put_locked(c);
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
	loader_line_putc_locked(c);
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
uint64_t loader_recording[3];   /* darwin-lisa: MiniIOMobileFramebuffer.cpp fills it (rec_secs=N) */
/* Set by MiniIOMobileFramebuffer.cpp at its first frame: vcputc() then only tees into the loader log, it no longer draws. */
int dlisa_vc_quiet;
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

/*
 * darwin-lisa bring-up: an interrupt-driven watchdog pet + timed return, called
 * from the vtimer IRQ handler (gic_vtimer.c) on every tick. A bring-up kernel
 * does not yet service delayed thread_calls, so hv_wdt_pet() and the
 * loader_return_after timer above never re-fire; this path instead rides the
 * one interrupt that is known to work. Timing is the architectural virtual
 * counter CNTVCT_EL0 / CNTFRQ_EL0 (no clock subsystem needed).
 */
static uint64_t lc_bringup_start;
static uint64_t lc_bringup_next_pet;
static uint32_t lc_bringup_return_secs;
static int lc_bringup_inited;

/* ---- bring-up diagnostic: does the vtimer survive WFI under the hv? ---- */
static void
lc_diag_hex(const char *tag, uint64_t v)
{
	const char *h = "0123456789abcdef";
	for (const char *p = tag; *p; p++) {
		loader_log_putc(*p);
	}
	for (int s = 60; s >= 0; s -= 4) {
		loader_log_putc(h[(v >> s) & 0xf]);
	}
}

static void
lc_diag_pet(void)
{
	register uint64_t x0 __asm__("x0") = VIRT_WDT_PET;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
}

/*
 * Called once from the vtimer handler after enough ticks. We are in IRQ context
 * (interrupts masked). Each round: arm the vtimer for ~1 s, WFI, then read back.
 * WFI wakes on a pending physical interrupt even while masked, so if the vtimer
 * fires the counter advances ~1 s and CNTV_CTL.ISTATUS becomes set. If the delta
 * stays ~0, the hypervisor is freezing the guest virtual counter at idle
 * (delivery can never happen). This isolates "hv freezes CNTVCT at WFI" from a
 * pure GIC delivery fault. Ends by handing the log back to the loader.
 */
void
loader_diag_wfi_probe(void)
{
	uint64_t freq = __builtin_arm_rsr64("CNTFRQ_EL0");

	loader_log_puts("\n--- diag: WFI vtimer probe (10 rounds, ~1s each) ---\n");
	for (int round = 0; round < 10; round++) {
		lc_diag_pet();
		uint64_t before = __builtin_arm_rsr64("CNTVCT_EL0");
		/* arm the EL1 virtual timer to fire in ~1 s and make sure it is on */
		__builtin_arm_wsr64("CNTV_TVAL_EL0", freq);
		__builtin_arm_wsr64("CNTV_CTL_EL0", 1 /* ENABLE, IMASK=0 */);
		__builtin_arm_dsb(DSB_SY);
		__builtin_arm_isb(ISB_SY);
		lc_diag_hex("[diag] armed round=0x", (uint64_t)round);
		loader_log_puts(" wfi...\n");	/* if the next line is missing, WFI never woke */
		__builtin_arm_wfi();
		uint64_t after = __builtin_arm_rsr64("CNTVCT_EL0");
		uint64_t ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
		uint64_t hppir = __builtin_arm_rsr64("ICC_HPPIR1_EL1");
		lc_diag_hex("[diag] round=0x", (uint64_t)round);
		lc_diag_hex(" dvct=0x", after - before);
		lc_diag_hex(" ctl=0x", ctl);
		lc_diag_hex(" hppir1=0x", hppir);
		loader_log_putc('\n');
	}
	loader_log_puts("--- diag done; returning ---\n");
	loader_return();	/* does not return */
}

/*
 * Keep the virtual timer ticking at >= 20 Hz. When the scheduler is idle it
 * arms a far-future decrementer, the vtimer stops, and this path (and the
 * timer-call queue) stall. Clamp the next fire to 50 ms so the timer keeps
 * firing: it feeds the watchdog here and gives rtclock_intr frequent chances
 * to run expired timer calls. A real, nearer deadline is left untouched.
 */
static void
lc_bringup_timer_floor(uint64_t freq)
{
	int32_t remaining = (int32_t)__builtin_arm_rsr64("CNTV_TVAL_EL0");
	int32_t floor = (int32_t)(freq / 20);
	if (remaining > floor) {
		__builtin_arm_wsr64("CNTV_TVAL_EL0", (uint64_t)(uint32_t)floor);
	}
}

void
loader_bringup_tick(void)
{
	uint64_t now = __builtin_arm_rsr64("CNTVCT_EL0");
	uint64_t freq = __builtin_arm_rsr64("CNTFRQ_EL0");

	if (freq == 0) {
		return;
	}
	/*
	 * With several cores (soc/smp) each takes its own timer: the watchdog, the heartbeat and the timed
	 * return are the boot processor's; the 50 ms floor is every core's.
	 */
	if (cpu_number() != boot_cpu_id) {
		lc_bringup_timer_floor(freq);
		return;
	}
	if (!lc_bringup_inited) {
		lc_bringup_inited = 1;
		lc_bringup_start = now;
		lc_bringup_next_pet = 0;
		PE_parse_boot_argn("loader_return_after", &lc_bringup_return_secs, sizeof(lc_bringup_return_secs));
	}

	uint64_t elapsed = (now - lc_bringup_start) / freq;	/* whole seconds */

	/* Feed the Qualcomm hypervisor watchdog every few seconds. */
	if (hv_wdt_enabled && elapsed >= lc_bringup_next_pet) {
		register uint64_t x0 __asm__("x0") = VIRT_WDT_PET;
		register uint64_t x1 __asm__("x1") = 0;
		register uint64_t x2 __asm__("x2") = 0;
		register uint64_t x3 __asm__("x3") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		loader_live.heartbeat++;
		lc_bringup_next_pet = elapsed + 5;
	}

	/* Hand control back once loader_return_after seconds have passed. */
	if (lc_bringup_return_secs != 0 && elapsed >= lc_bringup_return_secs) {
		loader_log_puts("\n--- loader_return_after expired (irq-driven) ---\n");
		loader_return();	/* does not return */
	}

	lc_bringup_timer_floor(freq);
}

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
