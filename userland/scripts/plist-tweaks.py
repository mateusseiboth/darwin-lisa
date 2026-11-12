#!/usr/bin/env python3
"""plist-tweaks.py [SYSROOT]: the LaunchDaemon changes the lisa USB boot needs, applied to out/sysroot-26 in place.

Idempotent. The first time a plist is changed its original is kept in out/plist-orig/ (next to sysroot-26), and
every run starts again from that original, so editing TWEAKS below and re-running is enough. Then re-image
(scripts/mkimage-trim.sh).

Besides the LaunchDaemons it adds two files (run before every image: mkimage-trim.sh calls it):
    System/Library/EventTimingProfiles/D22|D16.Default.plist  UIKit's default event timing profile for the boards the
                                                            loader can name (else a "Misconfigured Device" alert)
    private/etc/dlisa-ld-groups.conf                        the groups init26's boot-arg ld_disable= knows

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
    # wifid loads only on a device tree with a "wlan" node (or marconi-wifi, centauri-alpha...), which the
    # loader's does not have. init26 disables it for every boot without wifi=1 (init/wifi-net.c).
    "com.apple.wifid.plist": withoutKeys("_LimitLoadToDeviceTree"),
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


# The boards the loader's identity can name (target-type: "D22" in xnuloader/identity.h, the iPhone X; "D16" with
# loader/build-standalone.sh IDENTITY=, the iPhone 13 mini); UIKit looks for
# /System/Library/EventTimingProfiles/<board>.Default.plist and shows a "Misconfigured Device" alert without it.
BOARDS = ("D22", "D16")
TIMING_PROFILES = "System/Library/EventTimingProfiles"
# The 23F84 iPad image ships only J817/J818/J820/J821 profiles. Their Default is an empty dictionary (no board
# specific timing); their Touch and Pencil profiles describe 120 Hz ProMotion and Pencil hardware, which lisa's
# 60 Hz panel and Goodix touch are not, so only the Default is given to the board.
TIMING_TEMPLATE = "J817.Default.plist"


def saveDefaultTimingProfile(sysroot, board):
    folder = os.path.join(sysroot, TIMING_PROFILES)
    made = os.path.join(folder, f"{board}.Default.plist")
    shutil.copyfile(os.path.join(folder, TIMING_TEMPLATE), made)
    shutil.copystat(os.path.join(folder, TIMING_TEMPLATE), made)
    print(f"plist-tweaks: {TIMING_PROFILES}/{board}.Default.plist (from {TIMING_TEMPLATE})")
    return made


# init26's boot-arg ld_disable= reads its groups here (userland/init/ld-disable.c): "lean" is the whole of
# lean-daemons.txt, "lean1".. the same in chunks for bisecting the full image, "ui" the other UI shells.
LD_GROUPS = "private/etc/dlisa-ld-groups.conf"
LD_GROUP_SIZE = 10
UI_SHELLS = ("com.apple.ClarityBoard", "com.apple.checkerboard", "com.apple.devicerecoveryassistant",
             "com.apple.dt.AutomationModeUI")
# The "default" group: what init26 disables at every boot of the full image (ld_default=0 keeps them), the services
# that cannot work on this port and block the boot when they run.
#   CommCenter: no baseband here. It runs but never answers, and SpringBoard's main thread waits on it for good at
#   startup (-[SBTelephonyManager isInEmergencyCallbackMode] -> _CTServerConnectionGetEmergencyCallBackMode, a
#   synchronous XPC with retries): the full image's SpringBoard never drew a frame. Disabled, the lookup fails at
#   once, as on the lean image, and the full image reaches the home screen (2026-11-10, ld_disable=CommCenter).
#   PerfPowerServices (and its Extended half): the power/performance metrics daemon. After every swipe it kept one
#   core at 100 % for 20-40 s (kernel cpuload=5000 "tl: busy 4:PerfPowerService/1996"), and SpringBoard, which
#   reports its events to it, followed the finger at a crawl: Notification Center 25-90 s late, the app switcher's
#   transition still under way a minute later. Nothing here needs its metrics (2026-11-11, gpu-nc6 and gpu-nc7).
ALWAYS_DISABLED = ("com.apple.CommCenter", "com.apple.PerfPowerServices", "com.apple.PerfPowerServicesExtended")


def readLabels(sysroot, listing):
    labels = []
    with open(listing) as f:
        names = [line.strip() for line in f if line.strip() and not line.startswith("#")]
    for name in names:
        path = os.path.join(sysroot, LD, name)
        if not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            label = plistlib.load(f).get("Label")
        if label and label not in labels:
            labels.append(label)
    return labels


def readPresentLabels(sysroot, wanted):
    present = set()
    for name in os.listdir(os.path.join(sysroot, LD)):
        if not name.endswith(".plist"):
            continue
        with open(os.path.join(sysroot, LD, name), "rb") as f:
            present.add(plistlib.load(f).get("Label"))
    return [label for label in wanted if label in present]


def saveLdGroups(sysroot, listing, size=LD_GROUP_SIZE, alwaysDisabled=ALWAYS_DISABLED):
    labels = readLabels(sysroot, listing)
    chunks = [labels[i:i + size] for i in range(0, len(labels), size)]
    lines = ["# made by plist-tweaks.py from lean-daemons.txt: NAME label label ... (init26 ld_disable=NAME,...)",
             "lean " + " ".join(labels)]
    lines += [f"lean{n} " + " ".join(chunk) for n, chunk in enumerate(chunks, 1)]
    lines.append("ui " + " ".join(label for label in labels if label in UI_SHELLS))
    always = readPresentLabels(sysroot, alwaysDisabled)
    if always:
        lines.append("default " + " ".join(always))
    path = os.path.join(sysroot, LD_GROUPS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(path, 0o644)
    print(f"plist-tweaks: {LD_GROUPS}: {len(labels)} labels in {len(chunks)} chunks")
    return path


LOGGING_PREFS = "Library/Preferences/Logging/com.apple.system.logging.plist"


def saveLoggingPrivateData(sysroot):
    """libtrace reads this plist at every process's start: Enable-Private-Data makes os_log format its %{private}
    arguments instead of printing <private>, so the lines libdlisa copies to the console (dlisa_oslog=NAME) carry
    the paths and errors a diagnosis needs (PosterBoard's "Failed to migrate to Version 61 from 59 (<private>)")."""
    path = os.path.join(sysroot, LOGGING_PREFS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        plistlib.dump({"Enable-Private-Data": True}, f, fmt=plistlib.FMT_BINARY)
    os.chmod(path, 0o644)
    print(f"plist-tweaks: {LOGGING_PREFS}: Enable-Private-Data")
    return path


if __name__ == "__main__":
    for name, change in TWEAKS.items():
        tweak(name, change)
    for board in BOARDS:
        saveDefaultTimingProfile(SYSROOT, board)
    saveLdGroups(SYSROOT, os.path.join(os.path.dirname(os.path.abspath(__file__)), "lean-daemons.txt"))
    saveLoggingPrivateData(SYSROOT)
