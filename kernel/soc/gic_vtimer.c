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
			/* same sequence as the FIQ timer path in sleh_fiq() */
			getCpuDatap()->cpu_decrementer = -1;
			rtclock_intr(1);
		}
		/* other intids (SGIs for IPIs, SPIs for devices) are not wired yet */
		__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
		__asm__ volatile ("isb");
	}
}

void gic_vtimer_install(void);
void
gic_vtimer_install(void)
{
	ml_install_interrupt_handler(NULL, 0, NULL, gic_vtimer_handler, NULL);
}
