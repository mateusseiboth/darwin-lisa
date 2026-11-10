#!/usr/bin/env python3
# darwin-lisa: HakiOS's processor clock driver (bsd/dev/hakios.cpufreq.c, written for the POCO X5 Pro: same SoC,
# SM7325/sc7280, so the same EPSS and command-db addresses) as the optional tag `hakios_cpufreq`. Inert unless
# boot-arg cpufreq=1 (top frequency for the groups the kernel's cores are in; ddr= for the memory bus vote,
# default on with cpufreq=1, ddr=0 leaves the bus alone). The firmware leaves the cores at a modest clock, and this
# port runs everything on one core (cpus=1), so the boot of iOS 26's userland is CPU bound.
# NEEDS kernel/patches/cpu-idle-no-wfi.patch (or boot-arg wfi=0): with a fast core the kernel goes idle, WFI never
# wakes under this hypervisor, and the kernel hangs a few minutes into the boot (see ../display/README.md).
# ml_cpu_idle_time() (cpuload=N only) is not in this tree: a stub answering "not known" is added beside it.
# Idempotent; run in docker as root, then .work/run-rebuild.sh.
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"


def edit(path, old, new, tag):
    s = open(path).read()
    if new in s:
        print(f"  [skip] {tag}")
        return
    if s.count(old) != 1:
        print(f"  [FAIL] {tag}: anchor found {s.count(old)} times")
        sys.exit(2)
    open(path, "w").write(s.replace(old, new))
    print(f"  [ok]   {tag}")


shutil.copyfile(os.path.join(HERE, "hakios.cpufreq.c"), os.path.join(T, "bsd/dev/hakios.cpufreq.c"))
open(os.path.join(T, "bsd/dev/hakios.cpufreq_glue.c"), "w").write(
    "/* darwin-lisa: what hakios.cpufreq.c needs of HakiOS's kernel and this tree has not (cpuload=N only). */\n"
    "#include <stdbool.h>\n#include <stdint.h>\n\n"
    "bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);\n\n"
    "bool\nml_cpu_idle_time(int cpu_id, uint64_t *idle)\n{\n\t(void)cpu_id;\n\t(void)idle;\n\treturn false;\n}\n")
print("  [ok]   copied hakios.cpufreq.c + glue")
bf = os.path.join(T, "bsd/conf/files")
s = open(bf).read()
if "bsd/dev/hakios.cpufreq.c" not in s:
    open(bf, "a").write("\n# HakiOS processor clock (optional; cpufreq=1)\nbsd/dev/hakios.cpufreq.c\t\toptional hakios_cpufreq\n"
                        "bsd/dev/hakios.cpufreq_glue.c\t\toptional hakios_cpufreq\n")
    print("  [ok]   bsd/conf/files")
edit(os.path.join(T, "config/MASTER"),
     "options\t\tHAKIOS_DISPLAY\t# IOSurface + IOMobileFramebuffer (display=1)\t# <hakios_display>\n",
     "options\t\tHAKIOS_DISPLAY\t# IOSurface + IOMobileFramebuffer (display=1)\t# <hakios_display>\n"
     "options\t\tHAKIOS_CPUFREQ\t# processor clock (cpufreq=1)\t# <hakios_cpufreq>\n",
     "config/MASTER HAKIOS_CPUFREQ")
edit(os.path.join(T, "config/MASTER.arm64.bcm2837"),
     "hakios_display BSD_EXTRAS_DEV ]", "hakios_display hakios_cpufreq BSD_EXTRAS_DEV ]",
     "MASTER.arm64.bcm2837 += hakios_cpufreq")
print("### CPUFREQ WIRE DONE")
