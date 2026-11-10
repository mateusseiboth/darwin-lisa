#!/usr/bin/env python3
# darwin-lisa: wire the sensors PD's FastRPC file server into the xnu-work tree, on top of adsp-wire.py's tag
# `hakios_adsp` (no tag of its own: it lives and dies with the ADSP driver, and is inert without sensors_rpc=1):
#
#   bsd/dev/dlisa.fastrpc.c        the kernel's side: the SMMU context, the channel, the listener thread
#   bsd/dev/dlisa.fastrpc_proto.h  the wire formats and the file server (host-tested: test/run-fastrpc-tests.sh)
#   bsd/dev/dlisa.fastrpc.h        its presence switches hakios.adsp.c's hooks on (__has_include)
#   bsd/dev/hakios.adsp.c          (re-copied) the channel "fastrpcglink-apps-dsp" and the hooks
#   bsd/dev/dlisa.sensors_fs.h     generated: the staged sensor files linked in (.incbin of fw-sensors/sensors.fs;
#                                  Qualcomm's and the phone's files: never in git). FW_SENSORS= (empty) leaves them out,
#                                  and the server then says so and serves nothing.
#
# ORDER: after ../battery/battery-wire.py and adsp-wire.py (this needs their tag and files). Before it, stage the files:
#   python3 -I kernel/drivers/adsp/fw-sensors-stage.py stage      (or dump-persist first, with the phone in pmOS)
# Idempotent. XNU_WORK=<dir> points it at another tree (a dry run on a copy). Run in docker as root, then the rebuild
# (and grep logs/kernel.log for "error:": run-rebuild.sh hides compile failures).
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.environ.get("XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")
FW = os.environ.get("FW_SENSORS", "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/fw-sensors")


def read(path):
    with open(path) as f:
        return f.read()


def copy(name):
    src, dst = os.path.join(HERE, name), os.path.join(T, "bsd/dev", name)
    if os.path.exists(dst) and read(dst) == read(src):
        print(f"  [skip] bsd/dev/{name}: same")
        return
    shutil.copyfile(src, dst)
    print(f"  [ok]   bsd/dev/{name}")


def require(path, needle, why):
    if not os.path.exists(path) or needle not in read(path):
        print(f"  [FAIL] {why}: {needle!r} not in {path}")
        sys.exit(2)


print(f"### SENSORS (FastRPC) WIRE into {T}")
require(os.path.join(T, "config/MASTER"), "<hakios_adsp>", "the ADSP tag (run adsp-wire.py first)")
require(os.path.join(T, "bsd/conf/files"), "bsd/dev/dlisa.ssc.c", "the ADSP files (run adsp-wire.py first)")
require(os.path.join(T, "bsd/dev/hakios.platform.c"), "hakios_smmu_lock", "the SMMU lock shared with the Wi-Fi")

for name in ("dlisa.fastrpc_proto.h", "dlisa.fastrpc.h", "dlisa.fastrpc.c", "hakios.adsp.c"):
    copy(name)

files = os.path.join(T, "bsd/conf/files")
if "bsd/dev/dlisa.fastrpc.c" in read(files):
    print("  [skip] bsd/conf/files += dlisa.fastrpc.c: already applied")
else:
    with open(files, "a") as f:
        f.write("\n# darwin-lisa sensors: the sensors PD's FastRPC file server (optional with the ADSP; sensors_rpc=1)\n"
                "bsd/dev/dlisa.fastrpc.c\t\toptional hakios_adsp\n")
    print("  [ok]   bsd/conf/files += dlisa.fastrpc.c")

header = os.path.join(T, "bsd/dev/dlisa.sensors_fs.h")
if not FW:
    if os.path.exists(header):
        os.remove(header)
        print("  [ok]   bsd/dev/dlisa.sensors_fs.h removed: no sensor files built in")
    else:
        print("  [skip] no sensor files built in (FW_SENSORS empty)")
else:
    result = subprocess.run([sys.executable, "-I", os.path.join(HERE, "fw-sensors-stage.py"), "header", FW, header],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        print(f"  [FAIL] bsd/dev/dlisa.sensors_fs.h: stage the files first (fw-sensors-stage.py stage) into {FW}")
        sys.exit(2)
    print("  [skip] bsd/dev/dlisa.sensors_fs.h: the same already" if "the same already" in result.stdout else
          "  [ok]   bsd/dev/dlisa.sensors_fs.h: the sensor files built into the kernel")
print("### SENSORS WIRE DONE")
