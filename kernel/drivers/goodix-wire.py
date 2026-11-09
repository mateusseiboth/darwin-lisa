#!/usr/bin/env python3
# darwin-lisa: wire HakiOS's Goodix "Berlin" touch driver (hakios.goodix.c) into the
# xnu-12377 work tree as the OPTIONAL driver tag `hakios_goodix`. The standard input
# driver (hakios.input.c) already chains hakios_goodix_init(); enabling the tag swaps
# the hakios.stubs.c no-op (#ifndef HAKIOS_GOODIX) for the real driver. Idempotent.
# Run inside docker (root). Models hakios-wire.py / goodix pins are already lisa's
# (INT gpio81, RESET gpio105, SE13, I/O rail ldoc2); AVDD is lisa's ldoc7@3000 via the
# touch_avdd_ldo boot-arg (see report).
import os, shutil, sys

H = "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/xnu-src/xnu-8796.121.2"
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"

def copy(src, dst, tag):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    print(f"  [ok]   copied {tag}")

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

# 1. driver source + the FocalTech firmware-blob header it #includes.
copy(os.path.join(H, "bsd/dev/hakios.goodix.c"), os.path.join(T, "bsd/dev/hakios.goodix.c"), "bsd/dev/hakios.goodix.c")
copy(os.path.join(H, "bsd/dev/hakios.focaltech_fw.h"), os.path.join(T, "bsd/dev/hakios.focaltech_fw.h"), "bsd/dev/hakios.focaltech_fw.h")

# 2. bsd/conf/files: the optional driver, built only when the hakios_goodix tag is active.
bf = os.path.join(T, "bsd/conf/files")
with open(bf) as f:
    bfc = f.read()
if "bsd/dev/hakios.goodix.c" in bfc:
    print("  [skip] bsd/conf/files: hakios.goodix already listed")
else:
    with open(bf, "a") as f:
        f.write("\n# HakiOS Goodix/FocalTech touch driver (optional; ported from xnu-8796)\n"
                "bsd/dev/hakios.goodix.c\t\t\toptional hakios_goodix\n")
    print("  [ok]   bsd/conf/files += hakios.goodix.c")

# 3. config/MASTER: declare the HAKIOS_GOODIX option behind the <hakios_goodix> tag
#    (mirrors HakiOS config/MASTER.hakios; this is what #define's HAKIOS_GOODIX so the
#    hakios.stubs.c no-op drops out and the real hakios_goodix_init links).
edit(os.path.join(T, "config/MASTER"),
     "options\t\tAPFS\t\t# Apple File System (HakiOS built-in)\t# <apfs>\n",
     "options\t\tAPFS\t\t# Apple File System (HakiOS built-in)\t# <apfs>\n"
     "options\t\tHAKIOS_GOODIX\t# Goodix/FocalTech Berlin touch\t# <hakios_goodix>\n",
     "config/MASTER options HAKIOS_GOODIX")

# 4. config/MASTER.arm64.bcm2837: enable the hakios_goodix tag in the DEVELOPMENT config
#    (via BSD_DEV, the bsd/dev base the DEVELOPMENT config pulls in; same mechanism as
#    the apfs tag in FILESYS_BASE).
edit(os.path.join(T, "config/MASTER.arm64.bcm2837"),
     "#  BSD_DEV =        [ BSD_BASE config_netboot config_imgsrc_access pgo config_vnguard rosetta BSD_EXTRAS_DEV ]",
     "#  BSD_DEV =        [ BSD_BASE config_netboot config_imgsrc_access pgo config_vnguard rosetta hakios_goodix BSD_EXTRAS_DEV ]",
     "MASTER.arm64.bcm2837 BSD_DEV += hakios_goodix")

print("### GOODIX WIRE DONE")
