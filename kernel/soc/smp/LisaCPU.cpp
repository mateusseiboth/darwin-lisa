/* SPDX-License-Identifier: MIT */
/*
 * darwin-lisa: LisaCPU, the IOCPU driver of the SM7325 (lisa) cores.
 *
 * Port of HakiOS's GenericARMCPU (iokit/Kernel/GenericARMPE.cpp, xnu-8796, proven on this
 * phone with cpus=8) to xnu-12377, where it is matched to the /cpus nubs that ArmDTPlatform
 * publishes instead of being created by a platform expert of its own. The personality in
 * KernelConfigTables.cpp outbids UniXNU's ArmCpu (score 3000 against 2000): ArmCpu wants an
 * /arm-io/intc and a /psci node that the lisa loader does not write, and its secondary cores
 * are opt-in, so on lisa it never registered any core.
 *
 * Only with more than one core (boot-arg cpus=N, N > 1): with one, probe() declines and the
 * kernel runs exactly as before (no IOCPU at all; startup.c calls ml_set_max_cpus()).
 *
 * On xnu-12377 ml_processor_register() also boots the processor (processor_boot()): for the
 * boot processor that runs cpu_machine_init() -> initCPU(true) right away, for another one
 * cpu_start() -> startCPU() -> PSCI CPU_ON (osfmk/arm64/lisa_smp.c), and that core comes to
 * initCPU() by itself. The boot processor has to be registered first; a secondary whose nub
 * matches earlier waits in gPending until it is.
 */
extern "C" {
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <kern/cpu_number.h>
}
#include <machine/machine_routines.h>
#include <IOKit/IOCPU.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>

#if defined(BCM2837)

extern "C" {
void lisa_smp_init(void);
kern_return_t lisa_smp_start_cpu(int cpu);
void lisa_smp_cpu_running(int cpu);
void lisa_smp_signal(uint32_t phys_id, uint32_t sgi);
void lisa_smp_cpu_off(void) __dead2;
}

#define LISA_SGI_SIGNAL 0       /* cpu_signal(): gic_vtimer.c calls cpu_signal_handler() for it */

class LisaCPU : public IOCPU
{
	OSDeclareDefaultStructors(LisaCPU);

	uint32_t physID;        /* the device tree's "reg": the low affinity fields of the core's MPIDR */
	int      number;        /* the kernel's number for it */

	void registerNow(void);

public:
	virtual IOService * probe(IOService * provider, SInt32 * score) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	virtual void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	virtual void haltCPU(void) APPLE_KEXT_OVERRIDE;
	virtual void signalCPU(IOCPU * target) APPLE_KEXT_OVERRIDE;
	virtual const OSSymbol * getCPUName(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(LisaCPU, IOCPU);

static IOLock * gLisaCPULock;
static LisaCPU * gPending[MAX_CPUS];
static bool gBootRegistered;

/* The kernel's number for the processor of a /cpus nub, -1 if it is not one or not used (boot-arg cpus=N). */
static int
cpuNumberOfNub(IOService * nub)
{
	OSData * type = OSDynamicCast(OSData, nub->getProperty("device_type"));
	OSData * reg = OSDynamicCast(OSData, nub->getProperty("reg"));

	if (type == NULL || type->getLength() != 4 || memcmp(type->getBytesNoCopy(), "cpu", 4) != 0 ||
	    nub->getProperty("state") == NULL || reg == NULL || reg->getLength() < sizeof(uint32_t)) {
		return -1;
	}
	return ml_get_cpu_number(*(const uint32_t *)reg->getBytesNoCopy());
}

static IOLock *
lisaCPULock(void)
{
	IOLock * lock = __atomic_load_n(&gLisaCPULock, __ATOMIC_ACQUIRE);

	if (lock == NULL) {
		IOLock * fresh = IOLockAlloc();

		if (!__atomic_compare_exchange_n(&gLisaCPULock, &lock, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			IOLockFree(fresh);
		} else {
			lock = fresh;
		}
	}
	return lock;
}

IOService *
LisaCPU::probe(IOService * provider, SInt32 * score)
{
	int cpu = cpuNumberOfNub(provider);

	if (ml_get_topology_info()->num_cpus <= 1 || cpu < 0 || cpu >= MAX_CPUS) {
		return NULL;
	}
	return IOCPU::probe(provider, score);
}

/* With gLisaCPULock held: this processor into the kernel, which boots it. */
void
LisaCPU::registerNow(void)
{
	const ml_topology_cpu_t * topology = &ml_get_topology_info()->cpus[number];
	perfmon_interrupt_handler_func pmi_handler = NULL;
	ml_processor_info_t info;
	kern_return_t kr;

	bzero(&info, sizeof(info));
	info.cpu_id = (cpu_id_t)this;
	info.phys_id = physID;
	info.log_id = (uint32_t)number;
	info.cluster_id = topology->cluster_id;
	info.cluster_type = topology->cluster_type;
	info.l2_cache_id = topology->l2_cache_id;
	info.l2_cache_size = topology->l2_cache_size;
	info.l3_cache_id = topology->l3_cache_id;
	info.l3_cache_size = topology->l3_cache_size;
	setCPUState(kIOCPUStateUninitalized);
	kr = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler);
	if (kr != KERN_SUCCESS) {
		IOLog("LisaCPU: cpu %d (phys id 0x%x): ml_processor_register failed (%d)\n", number, physID, kr);
		return;
	}
	registerService();
	IOLog("LisaCPU: cpu %d (phys id 0x%x) registered%s\n", number, physID,
	    number == ml_get_boot_cpu_number() ? " (boot processor)" : "");
}

bool
LisaCPU::start(IOService * provider)
{
	IOLock * lock = lisaCPULock();

	number = cpuNumberOfNub(provider);
	if (number < 0 || lock == NULL || !IOCPU::start(provider)) {
		return false;
	}
	physID = ml_get_topology_info()->cpus[number].phys_id;
	setCPUNumber((UInt32)number);

	IOLockLock(lock);
	if (number == ml_get_boot_cpu_number()) {
		lisa_smp_init();
		registerNow();
		gBootRegistered = true;
		for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
			LisaCPU * pending = gPending[cpu];

			if (pending != NULL) {
				gPending[cpu] = NULL;
				pending->registerNow();
				pending->release();
			}
		}
	} else if (gBootRegistered) {
		registerNow();
	} else {
		retain();
		gPending[number] = this;
	}
	IOLockUnlock(lock);
	return true;
}

/*
 * On the processor itself, with its interrupts off, when it joins (or rejoins) the scheduler.
 * The interrupt controller and the timer were set up by pe_init_fiq() (arm_init / arm_init_cpu).
 * A processor takes no signals until it has handled one (cpu_signal_handler() lifts
 * SIGPdisabled; a secondary spins in processor_cpu_reinit() until then): send it its first.
 */
void
LisaCPU::initCPU(bool boot)
{
	setCPUState(kIOCPUStateRunning);
	lisa_smp_cpu_running(number);
	lisa_smp_signal(physID, LISA_SGI_SIGNAL);
}

/* On the processor itself, at the end of its way offline: it is to stop for good. */
void
LisaCPU::quiesceCPU(void)
{
	if (number != ml_get_boot_cpu_number()) {
		setCPUState(kIOCPUStateStopped);
		lisa_smp_cpu_off();
	}
}

kern_return_t
LisaCPU::startCPU(vm_offset_t start_paddr __unused, vm_offset_t arg_paddr __unused)
{
	return lisa_smp_start_cpu(number);
}

void
LisaCPU::haltCPU(void)
{
}

void
LisaCPU::signalCPU(IOCPU * target)
{
	lisa_smp_signal(((LisaCPU *)target)->physID, LISA_SGI_SIGNAL);
}

const OSSymbol *
LisaCPU::getCPUName(void)
{
	char name[32];

	snprintf(name, sizeof(name), "Primary%u", (unsigned)getCPUNumber());
	return OSSymbol::withCString(name);
}

#endif /* defined(BCM2837) */
