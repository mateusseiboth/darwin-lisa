#!/usr/bin/env python3
"""plist-tweaks.py [SYSROOT]: the LaunchDaemon changes the lisa USB boot needs, applied to out/sysroot-26 in place.

Idempotent. The first time a plist is changed its original is kept in out/plist-orig/ (next to sysroot-26), and
every run starts again from that original, so editing TWEAKS below and re-running is enough. Then re-image
(scripts/mkimage-trim.sh).

containermanagerd (EnablePressuredExit removed)
    launchd sends a clean, idle daemon with EnablePressuredExit a SIGTERM under memory pressure. This port runs
    with ~1.6 GB for everything and is under pressure from the start; containermanagerd took the SIGTERM, libxpc
    turned it into "Bogus check-in attempt" on its listener and dropped the peers, and every container lookup came
    back XPC_INTERRUPTED_REPLY: lsd aborted ("Could not get user container URL! Must abort.") and runningboardd,
    backboardd and SpringBoard waited on lsd for ever.
"""
import os
import plistlib
import shutil
import sys

SYSROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Dev/mobile/userland-26/out/sysroot-26")
ORIG = os.path.join(os.path.dirname(SYSROOT.rstrip("/")), "plist-orig")
LD = "System/Library/LaunchDaemons"


def withoutKeys(*keys):
    def apply(plist):
        for key in keys:
            plist.pop(key, None)
        return plist
    return apply


TWEAKS = {
    "com.apple.containermanagerd.plist": withoutKeys("EnablePressuredExit"),
    "com.apple.containermanagerd.system.plist": withoutKeys("EnablePressuredExit"),
}


def tweak(name, change):
    path = os.path.join(SYSROOT, LD, name)
    original = os.path.join(ORIG, name)
    if not os.path.exists(original):
        os.makedirs(ORIG, exist_ok=True)
        shutil.copy2(path, original)
    with open(original, "rb") as f:
        plist = plistlib.load(f)
    with open(path, "wb") as f:
        plistlib.dump(change(plist), f, fmt=plistlib.FMT_BINARY)
    shutil.copystat(original, path)
    print(f"plist-tweaks: {name}")


for name, change in TWEAKS.items():
    tweak(name, change)
