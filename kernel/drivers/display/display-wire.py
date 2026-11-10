#!/usr/bin/env python3
# darwin-lisa: wire the display stand-ins (HakiOS's MiniIOSurface + MiniIOMobileFramebuffer, ported to
# xnu-12377, vendored next to this script) into the xnu-work tree as the OPTIONAL tag `hakios_display`.
# They are inert unless boot-arg display=1 (hakios_display_init() in MiniIOMobileFramebuffer.cpp).
#
#   iokit/Kernel/MiniIOSurface.{cpp,h}        IOCoreSurfaceRoot + IOSurfaceRootUserClient (IOSurface.framework)
#   iokit/Kernel/MiniIOMobileFramebuffer.cpp  IOMobileFramebuffer "disp0" over the boot framebuffer (vinfo)
#
# HakiOS started both from its GenericARMPE; this tree keeps Apple's platform expert, so bsd_init calls
# hakios_display_init() next to the other hakios_*_init() calls (a no-op stub in hakios.stubs.c without the tag).
# The real MiniIOMobileFramebufferShow() replaces the no-op one adreno-wire.py put in hakios.adreno_glue.c.
# loader_recording[] in loader_console.c loses its `static` so the framebuffer's screen recording (rec_secs=N)
# reaches the loader's live block. Idempotent; run in docker as root, then .work/run-rebuild.sh.
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"


def copy(name):
    shutil.copyfile(os.path.join(HERE, name), os.path.join(T, "iokit/Kernel", name))
    print(f"  [ok]   copied iokit/Kernel/{name}")


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


# 1. sources (MiniDrivers.h is already in the tree, from wifi-wire.py, byte-identical to HakiOS's)
for name in ("MiniIOSurface.cpp", "MiniIOSurface.h", "MiniIOMobileFramebuffer.cpp"):
    copy(name)
if not os.path.exists(os.path.join(T, "iokit/Kernel/MiniDrivers.h")):
    shutil.copyfile(os.path.join(HERE, "..", "MiniDrivers.h"), os.path.join(T, "iokit/Kernel/MiniDrivers.h"))
    print("  [ok]   copied iokit/Kernel/MiniDrivers.h")

# 2. iokit/conf/files
append_once(os.path.join(T, "iokit/conf/files"), "iokit/Kernel/MiniIOSurface.cpp",
            "\n# HakiOS display stand-ins: IOSurface + IOMobileFramebuffer over the boot framebuffer (boot-arg display=1).\n"
            "iokit/Kernel/MiniIOSurface.cpp\t\toptional hakios_display iokitcpp\n"
            "iokit/Kernel/MiniIOMobileFramebuffer.cpp\toptional hakios_display iokitcpp\n",
            "iokit/conf/files += MiniIOSurface + MiniIOMobileFramebuffer")

# 3. config: option + tag
edit(os.path.join(T, "config/MASTER"),
     "options\t\tHAKIOS_WIFI\t# Wi-Fi of the WCN6750 (wifi=1, wifi_step=N)\t# <hakios_wifi>\n",
     "options\t\tHAKIOS_WIFI\t# Wi-Fi of the WCN6750 (wifi=1, wifi_step=N)\t# <hakios_wifi>\n"
     "options\t\tHAKIOS_DISPLAY\t# IOSurface + IOMobileFramebuffer (display=1)\t# <hakios_display>\n",
     "config/MASTER options HAKIOS_DISPLAY")
m = os.path.join(T, "config/MASTER.arm64.bcm2837")
if " hakios_display " in open(m).read():
    print("  [skip] MASTER.arm64.bcm2837 BSD_DEV += hakios_display: already applied")
else:
    edit(m, "hakios_goodix hakios_adreno hakios_wifi BSD_EXTRAS_DEV ]",
         "hakios_goodix hakios_adreno hakios_wifi hakios_display BSD_EXTRAS_DEV ]",
         "MASTER.arm64.bcm2837 BSD_DEV += hakios_display")

# 4. bsd_init: start them once devfs is up (IOKit has long been), before launchd
edit(os.path.join(T, "bsd/kern/bsd_init.c"),
     "\t\thakios_version_init();         /* kern.hakios.version, and the version in the boot log */\n",
     "\t\thakios_version_init();         /* kern.hakios.version, and the version in the boot log */\n"
     "\t\t{\n\t\t\textern void hakios_display_init(void);\n"
     "\t\t\thakios_display_init();  /* darwin-lisa: IOSurface + IOMobileFramebuffer (boot-arg display=1) */\n\t\t}\n",
     "bsd_init.c: call hakios_display_init")

# 5. the stub for a kernel without the tag
append_once(os.path.join(T, "bsd/dev/hakios.stubs.c"), "hakios_display_init",
            "\n#ifndef HAKIOS_DISPLAY\nvoid hakios_display_init(void);\nvoid hakios_display_init(void) {}\n#endif\n",
            "hakios.stubs.c += hakios_display_init stub")

# 6. the no-op MiniIOMobileFramebufferShow of the adreno glue goes: the real one is in MiniIOMobileFramebuffer.cpp
glue = os.path.join(T, "bsd/dev/hakios.adreno_glue.c")
if os.path.exists(glue):
    with open(glue) as f:
        g = f.read()
    start = g.find("void\nMiniIOMobileFramebufferShow(")
    if start >= 0:
        end = g.find("}\n", start) + 2
        g = g[:start] + "/* MiniIOMobileFramebufferShow: the real one is iokit/Kernel/MiniIOMobileFramebuffer.cpp (display-wire.py) */\n" + g[end:]
        with open(glue, "w") as f:
            f.write(g)
        print("  [ok]   hakios.adreno_glue.c: no-op MiniIOMobileFramebufferShow removed")
    else:
        print("  [skip] hakios.adreno_glue.c: no MiniIOMobileFramebufferShow body")

# 7. the screen recording's block, for the loader
edit(os.path.join(T, "osfmk/arm64/loader_console.c"),
     "static uint64_t loader_recording[3];\n",
     "uint64_t loader_recording[3];   /* darwin-lisa: MiniIOMobileFramebuffer.cpp fills it (rec_secs=N) */\n",
     "loader_console.c: loader_recording visible to the framebuffer")

print("### DISPLAY WIRE DONE")
