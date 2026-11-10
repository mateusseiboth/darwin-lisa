#!/usr/bin/env python3
# darwin-lisa: the battery as the optional tag `hakios_battery` (option HAKIOS_BATTERY, which drops the
# hakios.stubs.c no-ops for hakios_battery_init/_temperature/_report). Two files:
#   bsd/dev/dlisa.battery.c        lisa's VBAT/USB_IN through the PMK8350 ADC7 over SPMI (boot-arg battery=1)
#   iokit/Kernel/hakios.battery.cpp  HakiOS's HakiosBattery (IOPMPowerSource), unchanged from xnu-8796
# Inert unless battery=1. Idempotent (XNU_WORK=<dir>: a dry run on a copy); run in docker as root, then
# .work/run-rebuild.sh.
import os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.environ.get("XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")


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


def append_once(path, marker, text, tag):
    if marker in open(path).read():
        print(f"  [skip] {tag}")
        return
    open(path, "a").write(text)
    print(f"  [ok]   {tag}")


def copy(src, dst, tag):
    if os.path.exists(dst) and open(dst, "rb").read() == open(src, "rb").read():
        print(f"  [skip] {tag}: same")
        return
    shutil.copyfile(src, dst)
    print(f"  [ok]   {tag}")


copy(os.path.join(HERE, "dlisa.battery.c"), os.path.join(T, "bsd/dev/dlisa.battery.c"), "copied dlisa.battery.c")
copy(os.path.join(HERE, "hakios.battery.cpp"), os.path.join(T, "iokit/Kernel/hakios.battery.cpp"),
     "copied hakios.battery.cpp")
append_once(os.path.join(T, "bsd/conf/files"), "bsd/dev/dlisa.battery.c",
            "\n# darwin-lisa battery: VBAT through the PMK8350 ADC over SPMI (optional; battery=1)\n"
            "bsd/dev/dlisa.battery.c\t\toptional hakios_battery\n", "bsd/conf/files")
append_once(os.path.join(T, "iokit/conf/files"), "iokit/Kernel/hakios.battery.cpp",
            "\n# HakiOS IOPMPowerSource for the battery (optional; battery=1)\n"
            "iokit/Kernel/hakios.battery.cpp\toptional hakios_battery iokitcpp\n", "iokit/conf/files")
edit(os.path.join(T, "config/MASTER"),
     "options\t\tHAKIOS_CPUFREQ\t# processor clock (cpufreq=1)\t# <hakios_cpufreq>\n",
     "options\t\tHAKIOS_CPUFREQ\t# processor clock (cpufreq=1)\t# <hakios_cpufreq>\n"
     "options\t\tHAKIOS_BATTERY\t# battery through the PMIC's ADC (battery=1)\t# <hakios_battery>\n",
     "config/MASTER HAKIOS_BATTERY")
# (later wires put their tags between hakios_battery and BSD_EXTRAS_DEV, so "already there" is the tag on the line)
board = os.path.join(T, "config/MASTER.arm64.bcm2837")
if re.search(r"^#  BSD_DEV =.*\bhakios_battery\b", open(board).read(), re.M):
    print("  [skip] MASTER.arm64.bcm2837 += hakios_battery")
else:
    edit(board, "hakios_ufs BSD_EXTRAS_DEV ]", "hakios_ufs hakios_battery BSD_EXTRAS_DEV ]",
         "MASTER.arm64.bcm2837 += hakios_battery")
print("### BATTERY WIRE DONE")
