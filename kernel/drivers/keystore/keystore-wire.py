#!/usr/bin/env python3
# darwin-lisa: wire HakiOS's MiniAppleKeyStore (an AppleKeyStore stand-in for a device with no passcode, never locked;
# iokit/Kernel/MiniAppleKeyStore.cpp of HakiOS, vendored next to this script) into the xnu-work tree, under the
# optional tag `hakios_display` (already in the lisa config). Inert unless boot-arg keystore=1.
# bsd_init calls hakios_keystore_init() after hakios_display_init() and dlisa_sample_start(); hakios.stubs.c gets a no-op for a kernel
# without the tag. Idempotent; run in docker as root, after display-wire.py, then .work/run-rebuild.sh.
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"


def edit(path, old, new, tag):
    with open(path) as f:
        s = f.read()
    if new in s:
        print(f"  [skip] {tag}: already applied")
        return
    if s.count(old) != 1:
        print(f"  [FAIL] {tag}: anchor found {s.count(old)} times (need 1) in {path}")
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


shutil.copyfile(os.path.join(HERE, "MiniAppleKeyStore.cpp"), os.path.join(T, "iokit/Kernel/MiniAppleKeyStore.cpp"))
print("  [ok]   copied iokit/Kernel/MiniAppleKeyStore.cpp")

append_once(os.path.join(T, "iokit/conf/files"), "iokit/Kernel/MiniAppleKeyStore.cpp",
            "\n# HakiOS AppleKeyStore stand-in (boot-arg keystore=1).\n"
            "iokit/Kernel/MiniAppleKeyStore.cpp\toptional hakios_display iokitcpp\n",
            "iokit/conf/files += MiniAppleKeyStore")

edit(os.path.join(T, "bsd/kern/bsd_init.c"),
     "\t\t\t{ extern void dlisa_sample_start(void); dlisa_sample_start(); }  /* darwin-lisa diag: psample=NAME */\n",
     "\t\t\t{ extern void dlisa_sample_start(void); dlisa_sample_start(); }  /* darwin-lisa diag: psample=NAME */\n"
     "\t\t\textern void hakios_keystore_init(void);\n"
     "\t\t\thakios_keystore_init(); /* darwin-lisa: AppleKeyStore stand-in (boot-arg keystore=1) */\n",
     "bsd_init.c: call hakios_keystore_init")

append_once(os.path.join(T, "bsd/dev/hakios.stubs.c"), "hakios_keystore_init",
            "\n#ifndef HAKIOS_DISPLAY\nvoid hakios_keystore_init(void);\nvoid hakios_keystore_init(void) {}\n#endif\n",
            "hakios.stubs.c += hakios_keystore_init stub")
print("### KEYSTORE WIRE DONE")
