/* SPDX-License-Identifier: MIT */
/*
 * darwin-lisa: deliver the EL1 virtual-timer interrupt to XNU under the phone's
 * Qualcomm hypervisor.
 *
 * Stock xnu-12377 routes the virtual timer (PPI 27) as a GICv3 Group 0 interrupt
 * and handles it as an FIQ (sleh_fiq, ICC_IAR0_EL1). Under a third-party
 * hypervisor Group 0 / FIQ is taken to EL2, so the EL1 guest never sees the
 * timer and nothing timer- or scheduler-driven makes progress. The guest must
 * use Group 1 (IRQ), which reaches EL1 (sleh_irq -> the registered handler).
 *
 * pe_init_fiq() (patched) routes PPI 27 to Group 1 and calls gic_vtimer_install();
 * this handler acks ICC_IAR1_EL1, runs rtclock_intr for the vtimer, and EOIs.
 * Minimal port of HakiOS's gic_v3_irq.c handler (vtimer only; SPIs/IPIs later).
 */
#include <stdint.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>

#define GIC_INTID_VTIMER        27
#define GIC_INTID_SPURIOUS_MIN  1020

extern void loader_log_putc(char);	/* return-to-loader debug channel */
extern void loader_bringup_tick(void);	/* IRQ-driven watchdog pet + timed return */
extern void loader_diag_wfi_probe(void);	/* bring-up diag: vtimer-across-WFI test */
/* memory headroom on the heartbeat (pages of PAGE_SIZE; free + wired + total) */
extern unsigned int vm_page_free_count, vm_page_wire_count;
extern unsigned int vm_page_pages;

/* Proof the timer keeps firing: a counter printed on the first fire and every
 * GIC_VTIMER_HEARTBEAT fires after, so a one-shot timer is visibly distinct
 * from a periodically re-armed one. */
#define GIC_VTIMER_HEARTBEAT  500
static unsigned long gic_vtimer_ticks;

static void
gic_vtimer_mark(const char *tag, uint32_t value)
{
	const char *h = "0123456789abcdef";
	for (const char *p = tag; *p; p++) {
		loader_log_putc(*p);
	}
	for (int s = 28; s >= 0; s -= 4) {
		loader_log_putc(h[(value >> s) & 0xf]);
	}
	loader_log_putc('\n');
}

static void
gic_vtimer_handler(void *target __unused, void *refCon __unused, void *nub __unused, int source __unused)
{
	for (;;) {
		uint64_t iar = __builtin_arm_rsr64("ICC_IAR1_EL1");
		uint32_t intid = (uint32_t)(iar & 0xffffff);

		if (intid >= GIC_INTID_SPURIOUS_MIN) {
			break;		/* 1020..1023: spurious / no pending interrupt */
		}
		if (intid == GIC_INTID_VTIMER) {
			unsigned long t = ++gic_vtimer_ticks;
			if (t <= 4 || (t % GIC_VTIMER_HEARTBEAT) == 0) {
				gic_vtimer_mark("\n[GIC] vtimer tick 0x", (uint32_t)t);
				gic_vtimer_mark("[MEM] free pages 0x", vm_page_free_count);
				gic_vtimer_mark("[MEM] wired pages 0x", vm_page_wire_count);
				gic_vtimer_mark("[MEM] total pages 0x", vm_page_pages);
			}
			/* same sequence as the FIQ timer path in sleh_fiq() */
			getCpuDatap()->cpu_decrementer = -1;
			rtclock_intr(1);
			/* bring-up: pet the hv watchdog / hand back the log on time,
			 * without relying on delayed thread_calls (not yet serviced). */
			loader_bringup_tick();
		}
		/* other intids (SGIs for IPIs, SPIs for devices) are not wired yet */
		/* EOImode=1 (set in pe_init_fiq): split priority-drop from deactivate.
		 * Some hypervisor virtual CPU interfaces do not honor the combined
		 * EOImode=0 deactivate, wedging Group-1 delivery after a few interrupts;
		 * the explicit ICC_DIR_EL1 completes the deactivate. */
		__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);	/* priority drop */
		__builtin_arm_wsr64("ICC_DIR_EL1", iar);	/* deactivate */
		__asm__ volatile ("isb");
	}
}

void gic_vtimer_install(void);
void
gic_vtimer_install(void)
{
	ml_install_interrupt_handler(NULL, 0, NULL, gic_vtimer_handler, NULL);
}
