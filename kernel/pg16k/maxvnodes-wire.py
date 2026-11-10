#!/usr/bin/env python3
# darwin-lisa: room for more than 1024 vnodes. The lisa kernel is built as BCM2837 "xsmall" (a Raspberry Pi 3), which
# caps CONFIG_VNODES at 1024, so desiredvnodes (kern.maxvnodes) is 1024 whatever the memory. A full iOS 26 userland
# fills that in minutes once it runs at speed (cpufreq + several cores): the log fills with "vnode: table is full /
# 1024 desired, 1024 numvnodes, 0 free", and memorystatus kills daemon after daemon with JETSAM_REASON_VNODE (exit
# reason namespace 1 code 3), launchd relaunching each at once, ~900 kills in 20 minutes with every core at 100%.
# With the "medium" value unix_startup.c sizes it from memory as iOS does: memsize / 64 KB + 1024 (about 88k on the
# 5.4 GB window), clipped at 263168. (A loader /defaults "kern.maxvnodes" would override either.)
# Idempotent, --revert; run in docker as root, then .work/run-rebuild.sh.
import os
import sys

T = os.environ.get("XNU_WORK",
                   "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")
F = os.path.join(T, "config/MASTER.arm64.bcm2837")
OLD = "options   CONFIG_VNODES=1024\t\t# <xsmall>\n"
NEW = ("options   CONFIG_VNODES=263168\t\t# <xsmall>\t"
       "# darwin-lisa maxvnodes-wire.py: sized from memory (was 1024)\n")

s = open(F).read()
a, b = (NEW, OLD) if "--revert" in sys.argv else (OLD, NEW)
if b in s:
    print("  [skip] MASTER.arm64.bcm2837 CONFIG_VNODES: already", "reverted" if "--revert" in sys.argv else "applied")
elif s.count(a) == 1:
    open(F, "w").write(s.replace(a, b))
    print("  [ok]   MASTER.arm64.bcm2837 CONFIG_VNODES:", "1024" if "--revert" in sys.argv else "263168 (sized from memory)")
else:
    print(f"  [FAIL] anchor found {s.count(a)} times in {F}")
    sys.exit(2)
