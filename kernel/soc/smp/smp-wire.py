#!/usr/bin/env python3
# darwin-lisa: several processors on lisa (SM7325) for the xnu-12377 kernel. Port of HakiOS's GENERIC_PSCI_SMP
# (osfmk/arm64/psci_smp.c + GenericARMCPU, proven on this phone with cpus=8) onto this tree:
#
#   osfmk/arm64/lisa_smp.c      PSCI CPU_ON/CPU_OFF, the cache cleaning before CPU_ON, SGIs (vendored here)
#   iokit/Kernel/LisaCPU.cpp    the IOCPU driver of the /cpus nubs; registers the boot core first, then the rest
#   KernelConfigTables.cpp      a LisaCPU personality that outbids UniXNU's ArmCpu (3000 > 2000)
#   osfmk/arm64/start.s         reset_vector: a vector table of its own for a core started by PSCI (records a fault
#                               in lisa_smp_start_fault and switches the core off), a sane SCTLR, and the MPIDR
#                               match on Aff1|Aff0 (lisa numbers its cores in Aff1: 0x100 ... 0x700)
#   osfmk/arm64/cpu.c           no platform_cache_idle_enter() on BCM2837: a set/way flush of the data cache at
#                               every idle is slow (trapped by the hypervisor) and wrong with several cores
#
# Also needed, outside this script: kernel/patches/pe_fiq-vtimer-group1.patch (SGI 0 in Group 1, enabled, on every
# core) and kernel/soc/gic_vtimer.c (SGI 0 -> cpu_signal_handler()), both put in by kernel/patches/apply.sh.
# Inert with cpus=1 (LisaCPU::probe declines when the topology has one core). Boot-arg cpus=N (N > 1) turns it on;
# the loader then lists every core that is off (7, 6, 5, 4, 3, 2, 1 with bootcpu=7) and stays on core 0.
# Idempotent; run in docker as root, then .work/run-rebuild.sh.
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.environ.get("XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")


def edit(path, old, new, tag):
    with open(path) as f:
        s = f.read()
    if new in s:
        print(f"  [skip] {tag}: already applied")
        return
    n = s.count(old)
    if n != 1:
        print(f"  [FAIL] {tag}: anchor found {n} times (need 1) in {path}")
        sys.exit(2)
    with open(path, "w") as f:
        f.write(s.replace(old, new))
    print(f"  [ok]   {tag}")


def append_once(path, marker, text, tag):
    with open(path) as f:
        s = f.read()
    if marker in s:
        print(f"  [skip] {tag}: already applied")
        return
    with open(path, "a") as f:
        f.write(text)
    print(f"  [ok]   {tag}")


# 1. sources
shutil.copyfile(os.path.join(HERE, "lisa_smp.c"), os.path.join(T, "osfmk/arm64/lisa_smp.c"))
shutil.copyfile(os.path.join(HERE, "LisaCPU.cpp"), os.path.join(T, "iokit/Kernel/LisaCPU.cpp"))
print("  [ok]   copied osfmk/arm64/lisa_smp.c + iokit/Kernel/LisaCPU.cpp")

append_once(os.path.join(T, "osfmk/conf/files.arm64"), "osfmk/arm64/lisa_smp.c",
            "\n# darwin-lisa: several processors on lisa (PSCI CPU_ON + GICv3 SGIs; BCM2837 only)\n"
            "osfmk/arm64/lisa_smp.c\tstandard\n",
            "osfmk/conf/files.arm64 += lisa_smp.c")
append_once(os.path.join(T, "iokit/conf/files"), "iokit/Kernel/LisaCPU.cpp",
            "\n# darwin-lisa: the IOCPU driver of the lisa cores (BCM2837 only, cpus=N > 1)\n"
            "iokit/Kernel/LisaCPU.cpp\t\toptional iokitcpp\n",
            "iokit/conf/files += LisaCPU.cpp")

# 2. the personality, ahead of ArmCpu's
edit(os.path.join(T, "iokit/KernelConfigTables.cpp"),
     '    "("\n    "   {"\n    "     \'IOClass\'         = ArmCpu;"\n',
     '    "("\n'
     '    "   {"\n'
     '    "     \'IOClass\'         = LisaCPU;"\n'
     '    "     \'IOProviderClass\' = IOPlatformDevice;"\n'
     '    "     \'IOProbeScore\'    = 3000:32;"\n'
     '    "   },"\n'
     '    "   {"\n    "     \'IOClass\'         = ArmCpu;"\n',
     "KernelConfigTables.cpp += LisaCPU personality")

# 3. start.s: the reset vector of a core that PSCI starts
START = os.path.join(T, "osfmk/arm64/start.s")
edit(START,
     "LEXT(reset_vector)\n"
     "\t// Preserve x0 for start_first_cpu, if called\n"
     "\t// Unlock the core for debugging\n"
     "\tmsr\t\tOSLAR_EL1, xzr\n"
     "\tmsr\t\tDAIFSet, #(DAIFSC_ALL)\t\t\t\t// Disable all interrupts\n",
     "LEXT(reset_vector)\n"
     "\t// Preserve x0 for start_first_cpu, if called\n"
     "#if defined(BCM2837)\n"
     "\t/*\n"
     "\t * darwin-lisa (soc/smp): a core that the firmware has just started (PSCI CPU_ON, lisa_smp.c) has\n"
     "\t * no vectors at all, and its SCTLR is whatever PSCI left: vectors of its own first (not the boot\n"
     "\t * processor's), then only the bits that must be set, MMU and caches off, until common_start.\n"
     "\t */\n"
     "\tmsr\t\tDAIFSet, #(DAIFSC_ALL)\t\t\t\t// Disable all interrupts\n"
     "\tadrp\tx0, EXT(LowLisaSecondaryVectorBase)@page\n"
     "\tadd\t\tx0, x0, EXT(LowLisaSecondaryVectorBase)@pageoff\n"
     "\tmsr\t\tVBAR_EL1, x0\n"
     "\tisb\n"
     "\tMOV64\tx0, 0x30D00800\n"
     "\tmsr\t\tSCTLR_EL1, x0\n"
     "\tisb\n"
     "\t// Unlock the core for debugging\n"
     "\tmsr\t\tOSLAR_EL1, xzr\n"
     "#else /* defined(BCM2837) */\n"
     "\t// Unlock the core for debugging\n"
     "\tmsr\t\tOSLAR_EL1, xzr\n"
     "\tmsr\t\tDAIFSet, #(DAIFSC_ALL)\t\t\t\t// Disable all interrupts\n",
     "start.s: reset_vector head for PSCI-started cores")
edit(START,
     "\tadd     x0, x0, EXT(LowExceptionVectorBase)@pageoff\n"
     "\tmsr     VBAR_EL1, x0\n"
     "#endif\n",
     "\tadd     x0, x0, EXT(LowExceptionVectorBase)@pageoff\n"
     "\tmsr     VBAR_EL1, x0\n"
     "#endif\n"
     "#endif /* defined(BCM2837) */\n",
     "start.s: close the BCM2837 reset_vector branch")
edit(START,
     "#if HAS_CLUSTER\n"
     "\tand\t\tx0, x15, #0xFFFF\t\t\t\t\t// CPU number in Affinity0, cluster ID in Affinity1\n",
     "#if HAS_CLUSTER || defined(BCM2837)\n"
     "\t// (darwin-lisa: lisa numbers its cores in Affinity1 with Affinity0 zero; a cpu data's phys id is both,\n"
     "\t// as the device tree's \"reg\" has them)\n"
     "\tand\t\tx0, x15, #0xFFFF\t\t\t\t\t// CPU number in Affinity0, cluster ID in Affinity1\n",
     "start.s: MPIDR match on Aff1|Aff0")

VEC = "".join("\tb\t\tLlisa_secondary_early_exception\n" + ("\t.align\t7\n" if i < 15 else "") for i in range(16))
edit(START,
     "\t.align 12, 0\n\n#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)\n"
     "/*\n * Provide a global symbol",
     "#if defined(BCM2837)\n"
     "/*\n"
     " * darwin-lisa (soc/smp): the vectors of a core started by PSCI, from its first instruction (reset_vector)\n"
     " * until common_start gives it the kernel's. Not the boot loader's to return to: the core leaves a note\n"
     " * of what happened where lisa_smp_start_cpu() looks (struct lisa_smp_start_fault: valid, MPIDR, ESR, ELR,\n"
     " * FAR, LR, SP) and switches itself off again (PSCI CPU_OFF). VBAR holds a physical address here, so the\n"
     " * note is addressed physically; the store goes straight to memory with the MMU off and is sent there\n"
     " * when it is on.\n"
     " */\n"
     "\t.align\t11\n"
     "\t.globl EXT(LowLisaSecondaryVectorBase)\n"
     "LEXT(LowLisaSecondaryVectorBase)\n"
     + VEC +
     "\n"
     "\t.align\t3\n"
     "Llisa_secondary_early_exception:\n"
     "\tadrp\tx9, EXT(lisa_smp_start_fault)@page\n"
     "\tadd\t\tx9, x9, EXT(lisa_smp_start_fault)@pageoff\n"
     "\tmrs\t\tx10, MPIDR_EL1\n"
     "\tstr\t\tx10, [x9, #8]\n"
     "\tmrs\t\tx10, ESR_EL1\n"
     "\tstr\t\tx10, [x9, #16]\n"
     "\tmrs\t\tx10, ELR_EL1\n"
     "\tstr\t\tx10, [x9, #24]\n"
     "\tmrs\t\tx10, FAR_EL1\n"
     "\tstr\t\tx10, [x9, #32]\n"
     "\tstr\t\tlr, [x9, #40]\n"
     "\tmov\t\tx10, sp\n"
     "\tstr\t\tx10, [x9, #48]\n"
     "\tmov\t\tx10, #1\n"
     "\tstr\t\tx10, [x9]\n"
     "\tdc\t\tcivac, x9\n"
     "\tdsb\t\tsy\n"
     "\t// PSCI CPU_OFF, by the firmware's calling convention (lisa_smp_hvc)\n"
     "\tadrp\tx9, EXT(lisa_smp_hvc)@page\n"
     "\tadd\t\tx9, x9, EXT(lisa_smp_hvc)@pageoff\n"
     "\tldr\t\tw9, [x9]\n"
     "\tmov\t\tx0, #0x0002\n"
     "\tmovk\tx0, #0x8400, lsl #16\n"
     "\tcbnz\tw9, 1f\n"
     "\tsmc\t\t#0\n"
     "\tb\t\t2f\n"
     "1:\n"
     "\thvc\t\t#0\n"
     "2:\n"
     "\twfi\t\t\t\t\t\t\t\t\t\t// it does not return; if it does, stay here\n"
     "\tb\t\t2b\n"
     "#endif /* defined(BCM2837) */\n"
     "\t.align 12, 0\n\n#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)\n"
     "/*\n * Provide a global symbol",
     "start.s: LowLisaSecondaryVectorBase + early-exception note")

# 4. cpu.c: no set/way cache flush at every idle on lisa
edit(os.path.join(T, "osfmk/arm64/cpu.c"),
     "\tif (wfi) {\n#if !defined(APPLE_ARM64_ARCH_FAMILY)\n\t\tplatform_cache_idle_enter();\n#endif\n",
     "\tif (wfi) {\n"
     "#if !defined(APPLE_ARM64_ARCH_FAMILY) && !defined(BCM2837)\n"
     "\t\t/*\n"
     "\t\t * (darwin-lisa: not on lisa. A core keeps its cache while it waits, and this would empty it by set\n"
     "\t\t * and way every time: slow, wrong among several processors, and under the hypervisor every one\n"
     "\t\t * of those instructions is a trap.)\n"
     "\t\t */\n"
     "\t\tplatform_cache_idle_enter();\n#endif\n",
     "cpu.c: no platform_cache_idle_enter on BCM2837")

# 5. ml_cpu_idle_time() is lisa_smp.c's now: the "not known" stub of cpufreq-wire.py's glue goes
glue = os.path.join(T, "bsd/dev/hakios.cpufreq_glue.c")
if os.path.exists(glue) and "return false;" in open(glue).read():
    with open(glue, "w") as f:
        f.write("/* darwin-lisa: ml_cpu_idle_time() for hakios.cpufreq.c (cpuload=N) is in osfmk/arm64/lisa_smp.c. */\n")
    print("  [ok]   hakios.cpufreq_glue.c: stub ml_cpu_idle_time removed")
else:
    print("  [skip] hakios.cpufreq_glue.c: no stub")

# 6. loader_log_line() for real (it was a no-op stand-in), so the "tl:" lines of cpuload=MS reach the loader log
shutil.copyfile(os.path.join(HERE, "..", "..", "drivers", "hakios.lisa_glue.c"), os.path.join(T, "bsd/dev/hakios.lisa_glue.c"))
print("  [ok]   copied bsd/dev/hakios.lisa_glue.c (loader_log_line writes the loader log)")

print("### SMP WIRE DONE")
