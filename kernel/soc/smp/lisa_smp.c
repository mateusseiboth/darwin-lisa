/* SPDX-License-Identifier: MIT */
/*
 * darwin-lisa: more than one processor on the SM7325 (lisa) under Qualcomm's hypervisor.
 *
 * Port of HakiOS's osfmk/arm64/psci_smp.c (xnu-8796, proven on this phone with cpus=8) to
 * xnu-12377. The base platform (BCM2837) has no USE_APPLEARMSMP and no closed CPU driver,
 * so the other cores are started here through the firmware's power interface (PSCI CPU_ON,
 * SMC or HVC as /chosen/psci-method says) at the kernel's own reset vector
 * (LowResetVectorBase in start.s). Signals between processors are GICv3 software-generated
 * interrupts: SGI 0 carries cpu_signal() (gic_vtimer.c hands it to cpu_signal_handler()).
 *
 * The IOKit side, LisaCPU (iokit/Kernel/LisaCPU.cpp), registers every core of the device
 * tree with ml_processor_register(); on xnu-12377 that call also boots the processor
 * (processor_boot() -> cpu_start() -> LisaCPU::startCPU() -> lisa_smp_start_cpu()).
 *
 * A new core begins with its MMU and caches off and reads memory, not the running cores'
 * caches: under this hypervisor a dirty line in another core's cache is simply not there
 * yet. So everything the reset path reads before its MMU is on is cleaned to the point of
 * coherency before CPU_ON.
 *
 * The boot loader (xnuloader) stays on core 0, petting the hypervisor watchdog and holding
 * the USB log. It lists only cores that are off in /cpus, so core 0 is never the kernel's.
 */
#include <mach/mach_types.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>

#if defined(BCM2837)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u

#define LISA_SMP_START_SECONDS  5       /* how long a started core is waited for */

#define LISA_SMP_CPU_ASKED      1       /* CPU_ON granted */
#define LISA_SMP_CPU_RUNNING    3       /* LisaCPU::initCPU() ran on it */
#define LISA_SMP_CPU_OFF        4

extern void loader_log_putc(char c);

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t lisa_smp_hvc;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Llisa_secondary_early_exception in start.s). That core writes it with its cache off; it has
 * a cache line to itself so that looking at memory does not disturb anything else.
 */
struct lisa_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} lisa_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct lisa_smp_start_fault) == 64, "one cache line");

static uint32_t lisa_smp_cpu_stage[MAX_CPUS];
static bool     lisa_smp_inited;

void lisa_smp_init(void);
kern_return_t lisa_smp_start_cpu(int cpu);
void lisa_smp_cpu_running(int cpu);
void lisa_smp_signal(uint32_t phys_id, uint32_t sgi);
void lisa_smp_cpu_off(void) __dead2;

static void
lisa_smp_log(const char *line)
{
	for (const char *c = line; *c; c++) {
		loader_log_putc(*c);
	}
}

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (lisa_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	}
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

/* Thread context, before the first CPU_ON: how the firmware is to be called. */
void
lisa_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (lisa_smp_inited) {
		return;
	}
	lisa_smp_inited = true;
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		lisa_smp_hvc = 1;
	}
}

/* A processor other than the boot processor has come all the way (LisaCPU::initCPU()). */
void
lisa_smp_cpu_running(int cpu)
{
	if (cpu >= 0 && cpu < MAX_CPUS) {
		os_atomic_store(&lisa_smp_cpu_stage[cpu], LISA_SMP_CPU_RUNNING, release);
	}
}

/*
 * A software-generated interrupt for the processor with that MPIDR (its two low affinity fields,
 * as in the device tree's "reg"; lisa numbers its cores in Aff1: 0x100 ... 0x700).
 * ICC_SGI1R_EL1: Aff1 in [23:16], the interrupt in [27:24], a bit per Aff0 in [15:0].
 */
void
lisa_smp_signal(uint32_t phys_id, uint32_t sgi)
{
	uint64_t aff0 = phys_id & 0xff, aff1 = (phys_id >> 8) & 0xff;

	__builtin_arm_dsb(DSB_ISHST);
	__builtin_arm_wsr64("ICC_SGI1R_EL1", ((uint64_t)sgi << 24) | (aff1 << 16) | (1ULL << (aff0 & 15)));
	__builtin_arm_isb(ISB_SY);
}

static void
lisa_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * reached LisaCPU::initCPU(), and says what became of it if it has not: on a phone the log is
 * all there is to go by.
 */
kern_return_t
lisa_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t deadline;
	int64_t status;
	char line[256];

	lisa_smp_init();

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, lisa_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	lisa_smp_start_fault.valid = 0;
	lisa_smp_clean_segment("__TEXT");
	lisa_smp_clean_segment("__DATA_CONST");
	lisa_smp_clean_segment("__TEXT_EXEC");
	lisa_smp_clean_segment("__DATA");
	lisa_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&lisa_smp_cpu_stage[cpu], LISA_SMP_CPU_ASKED, release);
	status = psci_call(PSCI_CPU_ON, phys_id, entry, 0);
	if (status != 0) {
		os_atomic_store(&lisa_smp_cpu_stage[cpu], 0, release);
		snprintf(line, sizeof(line), "\nlisa-smp: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		lisa_smp_log(line);
		return KERN_FAILURE;
	}

	clock_interval_to_deadline(LISA_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&lisa_smp_cpu_stage[cpu], acquire);
		if (stage >= LISA_SMP_CPU_RUNNING) {
			snprintf(line, sizeof(line), "\nlisa-smp: cpu %d (MPIDR 0x%x) is up\n", cpu, phys_id);
			lisa_smp_log(line);
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&lisa_smp_start_fault, sizeof(lisa_smp_start_fault));
		if (lisa_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	} while (mach_absolute_time() < deadline);

	if (lisa_smp_start_fault.valid != 0) {
		struct lisa_smp_start_fault *f = &lisa_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		snprintf(line, sizeof(line), "\nlisa-smp: cpu %d (MPIDR 0x%llx) took an exception on its way in and switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (kernel VA: ELR 0x%llx LR 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
	} else {
		snprintf(line, sizeof(line), "\nlisa-smp: cpu %d (MPIDR 0x%x) was started %d s ago and has not come up (stage %u)\n",
		    cpu, phys_id, LISA_SMP_START_SECONDS, stage);
	}
	lisa_smp_log(line);
	/* processor_dostartup() takes cpu_start() at its word: a core that never comes is waited for later, if at all */
	return KERN_FAILURE;
}

/* The calling processor switches itself off for good. */
void
lisa_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 0);
	__builtin_arm_isb(ISB_SY);
	if (cpu_number() < MAX_CPUS) {
		os_atomic_store(&lisa_smp_cpu_stage[cpu_number()], LISA_SMP_CPU_OFF, release);
	}
	__builtin_arm_dsb(DSB_SY);
	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

/*
 * How long processor cpu_id has been idle since it started, in mach_absolute_time() units: boot-arg cpuload=MS
 * of the processor clock driver (hakios.cpufreq.c) prints each core's share of busy time from it. (HakiOS had
 * this in machine_routines.c; cpufreq-wire.py's glue used to answer "not known".)
 */
bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);

bool
ml_cpu_idle_time(int cpu_id, uint64_t *idle)
{
	struct recount_usage usage = { 0 };

	if (cpu_id < 0 || cpu_id >= MAX_CPUS || processor_array[cpu_id] == PROCESSOR_NULL) {
		return false;
	}
	recount_processor_usage(&processor_array[cpu_id]->pr_recount, &usage, idle);
	return true;
}

#endif /* defined(BCM2837) */
