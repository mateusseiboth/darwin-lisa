#!/usr/bin/env python3
"""rtc-wifi-userland-wire.py: the userland half of the real-time clock and the Wi-Fi join, folded into the sources the
root image is built from. Run it once before building init26 and the root (then the usual build-init.sh in the
toolchain image, and mkimage-trim.sh). Idempotent: [skip] for what is there already, [FAIL] (exit 2) when an anchor
is not found exactly once. DARWIN_LISA=<dir> points it at a copy of darwin-lisa (a dry run).

  userland/init/init-26.c       includes rtc-offset.c, wifi-net.c (pure, host tests in init/test) and rtc-wifi-boot.c
                                (their system calls); main() calls applySavedRtcOffset(), startRtcKeeper() and
                                giveWifiNetwork() once /private/var is there; ld_disable= reads the boot-args through
                                withWifiServices(): wifid is disabled for the boot unless wifi=1 (and not wifid=0)
  userland/scripts/lean-daemons.txt   com.apple.wifid.plist no longer left out of the image (init26 gates it instead);
                                wifiFirmwareLoader stays out (it waits for Broadcom's AppleBCMWLANCore)
  userland/scripts/plist-tweaks.py    com.apple.wifid.plist without _LimitLoadToDeviceTree: launchd loads wifid only on a
                                device tree with a "wlan" node (or marconi-wifi, centauri-alpha...), which the
                                loader's tree does not have

Everything is inert without the boot-args rtc=1 / wifi=1 (wifid stays disabled then, as it was absent before).
"""
import os
import sys

ROOT = os.environ.get("DARWIN_LISA", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def fail(tag, why):
    print(f"  [FAIL] {tag}: {why}")
    sys.exit(2)


def edit(relative, old, new, tag):
    name = os.path.join(ROOT, relative)
    with open(name) as f:
        text = f.read()
    if new in text:
        print(f"  [skip] {tag}: already applied")
        return
    if text.count(old) != 1:
        fail(tag, f"anchor found {text.count(old)} times (need 1) in {relative}")
    with open(name, "w") as f:
        f.write(text.replace(old, new))
    print(f"  [ok]   {tag}")


def drop_line(relative, line, tag):
    name = os.path.join(ROOT, relative)
    with open(name) as f:
        lines = f.read().splitlines(keepends=True)
    kept = [each for each in lines if each.strip() != line]
    if len(kept) == len(lines):
        print(f"  [skip] {tag}: already applied")
        return
    with open(name, "w") as f:
        f.write("".join(kept))
    print(f"  [ok]   {tag}")


INIT = "userland/init/init-26.c"
edit(INIT, '#include "identity-stamp.c"\n',
     '#include "identity-stamp.c"\n#include "rtc-offset.c"\n#include "wifi-net.c"\n',
     "init-26.c: rtc-offset.c + wifi-net.c included")
edit(INIT, "count = buildDisabledPlist(bootArgs(), ",
     "count = buildDisabledPlist(withWifiServices(bootArgs()), ",
     "init-26.c: wifid disabled unless wifi=1")
edit(INIT, "static void\nhandOff(char **envp)\n",
     '#include "rtc-wifi-boot.c"\n\nstatic void\nhandOff(char **envp)\n',
     "init-26.c: rtc-wifi-boot.c included")
edit(INIT, "\tmountVar();\n",
     "\tmountVar();\n"
     "\t/* darwin-lisa: the clock's kept offset (rtc=1) and the kept Wi-Fi network (wifi=1), init/rtc-wifi-boot.c */\n"
     "\tapplySavedRtcOffset();\n\tstartRtcKeeper();\n\tgiveWifiNetwork();\n",
     "init-26.c: main() applies the clock's offset and gives the network")
drop_line("userland/scripts/lean-daemons.txt", "com.apple.wifid.plist", "lean-daemons.txt: wifid is in the image")
edit("userland/scripts/plist-tweaks.py",
     '    "com.apple.containermanagerd.system.plist": withoutKeys("EnablePressuredExit"),\n',
     '    "com.apple.containermanagerd.system.plist": withoutKeys("EnablePressuredExit"),\n'
     '    # wifid loads only on a device tree with a "wlan" node (or marconi-wifi, centauri-alpha...), which the\n'
     '    # loader\'s does not have. init26 disables it for every boot without wifi=1 (init/wifi-net.c).\n'
     '    "com.apple.wifid.plist": withoutKeys("_LimitLoadToDeviceTree"),\n',
     "plist-tweaks.py: wifid without _LimitLoadToDeviceTree")
print("### RTC + WIFI USERLAND WIRE DONE")
