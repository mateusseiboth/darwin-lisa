#!/usr/bin/env python3
# darwin-lisa: wire the audio DSP (ADSP) of lisa into the xnu-work tree as the OPTIONAL tag `hakios_adsp`: the DSP
# booted through TrustZone PIL, GLINK over SMEM, QRTR, the battery manager (pmic_glink battmgr) as the real fuel
# gauge, and the sensors' client on the DSP's sensor core.
#
#   bsd/dev/hakios.adsp.c           HakiOS's drivers/audio/hakios.adsp.c with the darwin-lisa changes (its header)
#   bsd/dev/dlisa.ssc.c             the SSC (sensor core) client and sensors_fake (kern.hakios.sensors)
#   bsd/dev/dlisa.battery.c         (re-copied) the SPMI ADC estimate, now yielding to the fuel gauge
#   iokit/Kernel/hakios.battery.cpp (re-copied) HakiosBattery, now with BatteryHealth / ChargeStatus
#
# Needs the tag hakios_battery (../battery/battery-wire.py): hakios.adsp.c calls dlisa_battery_note_health() and
# hakios_battery_report(), and HakiosBattery calls dlisa_battery_health(), all in dlisa.battery.c. The no-op
# hakios_adsp_init/_stop/_quiesce for a kernel without the tag are already in bsd/dev/hakios.stubs.c (#ifndef
# HAKIOS_ADSP), and hakios.input.c already calls hakios_adsp_init(); nothing else in the tree changes.
# Inert at run time unless boot-arg adsp=1 (or adsp_step=N), battmgr=1, sensors=1 or sensors_fake=1.
# ORDER: after ../battery/battery-wire.py (it copies the two battery files too: run this one after it, or the
# older copies win). Idempotent. XNU_WORK=<dir> points it at another tree (a dry run on a copy). Run in docker as
# root, then the rebuild (and grep logs/kernel.log for "error:": run-rebuild.sh hides compile failures).
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVERS = os.path.dirname(HERE)
T = os.environ.get("XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")


def read(path):
    with open(path) as f:
        return f.read()


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def copy(src, dst, tag):
    if os.path.exists(dst) and read(dst) == read(src):
        print(f"  [skip] {tag}: same")
        return
    shutil.copyfile(src, dst)
    print(f"  [ok]   {tag}")


def append_once(path, marker, text, tag):
    if marker in read(path):
        print(f"  [skip] {tag}: already applied")
        return
    with open(path, "a") as f:
        f.write(text)
    print(f"  [ok]   {tag}")


def require(path, needle, why):
    if not os.path.exists(path) or needle not in read(path):
        print(f"  [FAIL] {why}: {needle!r} not in {path}")
        sys.exit(2)


print(f"### ADSP WIRE into {T}")
require(os.path.join(T, "config/MASTER"), "<hakios_battery>", "the battery tag (run ../battery/battery-wire.py first)")
require(os.path.join(T, "bsd/conf/files"), "bsd/dev/dlisa.battery.c", "the battery file (run ../battery/battery-wire.py first)")
require(os.path.join(T, "bsd/dev/hakios.stubs.c"), "#ifndef HAKIOS_ADSP", "the ADSP stand-ins")
require(os.path.join(T, "bsd/dev/hakios.input.c"), "hakios_adsp_init();", "the ADSP's start")
require(os.path.join(T, "bsd/dev/hakios.platform.c"), "hakios_rsc_lock", "the locks shared with the Wi-Fi")

copy(os.path.join(HERE, "hakios.adsp.c"), os.path.join(T, "bsd/dev/hakios.adsp.c"), "bsd/dev/hakios.adsp.c")
copy(os.path.join(HERE, "dlisa.ssc.c"), os.path.join(T, "bsd/dev/dlisa.ssc.c"), "bsd/dev/dlisa.ssc.c")
copy(os.path.join(DRIVERS, "battery/dlisa.battery.c"), os.path.join(T, "bsd/dev/dlisa.battery.c"), "bsd/dev/dlisa.battery.c")
copy(os.path.join(DRIVERS, "battery/hakios.battery.cpp"), os.path.join(T, "iokit/Kernel/hakios.battery.cpp"),
     "iokit/Kernel/hakios.battery.cpp")

append_once(os.path.join(T, "bsd/conf/files"), "bsd/dev/hakios.adsp.c",
            "\n# darwin-lisa ADSP: PIL + GLINK + QRTR, battmgr fuel gauge, sensor core client (optional; adsp=1)\n"
            "bsd/dev/hakios.adsp.c\t\toptional hakios_adsp\n"
            "bsd/dev/dlisa.ssc.c\t\toptional hakios_adsp\n", "bsd/conf/files += hakios.adsp.c dlisa.ssc.c")

master = os.path.join(T, "config/MASTER")
s = read(master)
if "<hakios_adsp>" in s:
    print("  [skip] config/MASTER HAKIOS_ADSP: already applied")
else:
    lines = s.split("\n")
    at = [i for i, line in enumerate(lines) if "<hakios_battery>" in line]
    if len(at) != 1:
        print(f"  [FAIL] config/MASTER: the HAKIOS_BATTERY line found {len(at)} times")
        sys.exit(2)
    lines.insert(at[0] + 1, "options\t\tHAKIOS_ADSP\t# audio DSP, fuel gauge, sensors (adsp=1 battmgr=1 sensors=1)\t# <hakios_adsp>")
    write(master, "\n".join(lines))
    print("  [ok]   config/MASTER HAKIOS_ADSP")

board = os.path.join(T, "config/MASTER.arm64.bcm2837")
s = read(board)
dev = [line for line in s.split("\n") if re.search(r"BSD_DEV\s*=\s*\[", line)]
if len(dev) != 1:
    print(f"  [FAIL] MASTER.arm64.bcm2837: the BSD_DEV line found {len(dev)} times")
    sys.exit(2)
if re.search(r"\bhakios_adsp\b", dev[0]):
    print("  [skip] MASTER.arm64.bcm2837 hakios_adsp: already applied")
elif " BSD_EXTRAS_DEV ]" not in dev[0] or "hakios_battery" not in dev[0]:
    print("  [FAIL] MASTER.arm64.bcm2837: the BSD_DEV line has no ' BSD_EXTRAS_DEV ]' or no hakios_battery")
    sys.exit(2)
else:
    write(board, s.replace(dev[0], dev[0].replace(" BSD_EXTRAS_DEV ]", " hakios_adsp BSD_EXTRAS_DEV ]")))
    print("  [ok]   MASTER.arm64.bcm2837 += hakios_adsp")

# 5. the DSP's firmware built into the kernel (darwin-lisa, the evening integration): the root has ~6 MB free before
#    the data volume in super, and the firmware is 20.8 MB (10.5 MB compressed). Copied into bsd/dev/hakios.adsp_fw/
#    and assembled in by bsd/dev/hakios.adsp_fw.h (.incbin; both generated here, never in git: Qualcomm's blobs).
#    hakios.adsp.c still asks the RAM disks and the flash root's fw/ first, so a firmware RAM disk overrides it.
#    FW_ADSP=DIR picks the staged set (default fw-adsp-stage.py's); FW_ADSP= (empty) removes the built-in set.
FW = os.environ.get("FW_ADSP", "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/fw-adsp")
fw_copy = os.path.join(T, "bsd/dev/hakios.adsp_fw")
fw_header = os.path.join(T, "bsd/dev/hakios.adsp_fw.h")
if not FW:
    if os.path.exists(fw_header):
        os.remove(fw_header)
        print("  [ok]   bsd/dev/hakios.adsp_fw.h removed: no firmware built in (adsp=1 reads /fw of the root)")
    else:
        print("  [skip] no firmware built in (FW_ADSP empty)")
else:
    names = ["adsp.mdt"] + sorted(n for n in os.listdir(FW) if n.startswith("adsp.b") and len(n) == 8)
    os.makedirs(fw_copy, exist_ok=True)
    changed = 0
    for name in names:
        src, dst = os.path.join(FW, name), os.path.join(fw_copy, name)
        with open(src, "rb") as f:
            data = f.read()
        if os.path.exists(dst) and open(dst, "rb").read() == data:
            continue
        with open(dst, "wb") as f:
            f.write(data)
        changed += 1
    print(f"  [ok]   bsd/dev/hakios.adsp_fw/: {changed} of {len(names)} files copied from {FW}" if changed else
          f"  [skip] bsd/dev/hakios.adsp_fw/: the same {len(names)} files already")
    result = subprocess.run([sys.executable, "-I", os.path.join(HERE, "fw-adsp-stage.py"), "header", fw_copy, fw_header],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        print("  [FAIL] bsd/dev/hakios.adsp_fw.h: fw-adsp-stage.py header refused the set")
        sys.exit(2)
    print("  [skip] bsd/dev/hakios.adsp_fw.h: the same already" if "the same already" in result.stdout else
          "  [ok]   bsd/dev/hakios.adsp_fw.h: the firmware built into the kernel")
print("### ADSP WIRE DONE")
