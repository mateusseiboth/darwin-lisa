#!/usr/bin/env python3
# darwin-lisa: this port has no IOWatchdog kext (Apple's AppleARMWatchdogTimer).
# watchdogd looks it up, and when it is missing it __os_crash()es (SIGTRAP) unless
# kern.hv_vmm_present says it runs in a VM, in which case it logs "detected virtual
# machine environment and no watchdog KEXT found, exiting..." and exits 0 (its plist
# has KeepAlive.SuccessfulExit = false, so launchd leaves it down). launchd marks it
# _PanicOnCrash, so the SIGTRAP loop panicked the kernel. Answer hv_vmm_present = 1
# to watchdogd only; everything else still sees the device tree's real value.
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/bsd/kern/hvg_sysctl.c"

ANCHOR = "\tvmm_present = !!vmm_present;\n\treturn SYSCTL_OUT(req, &vmm_present, sizeof(vmm_present));\n"
ADD = """\t/* darwin-lisa: no IOWatchdog kext; let watchdogd take its VM exit path */
\tif (!vmm_present && req->p != NULL && strncmp(proc_best_name(req->p), "watchdogd", 10) == 0) {
\t\tvmm_present = 1;
\t}
"""

with open(F) as f:
    d = f.read()
if "--revert" in sys.argv:
    d = d.replace(ADD + ANCHOR, ANCHOR)
    print("watchdogvm: reverted")
elif ADD in d:
    print("watchdogvm: already present")
elif ANCHOR in d:
    d = d.replace(ANCHOR, ADD + ANCHOR, 1)
    print("watchdogvm: patched")
else:
    print("watchdogvm: anchor missing"); sys.exit(1)
with open(F, "w") as f:
    f.write(d)
