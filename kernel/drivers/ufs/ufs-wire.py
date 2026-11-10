#!/usr/bin/env python3
# darwin-lisa: HakiOS's UFS block driver (bsd/dev/hakios.ufs.c + hakios.ufshc.c, vendored here and rebased onto
# xnu-12377) as the optional tag `hakios_ufs`, and the root on a GPT partition of the phone's flash.
#
#   hakios.ufs.c / hakios.ufshc.c / .h   /dev/ufs_NAME + /dev/rufs_NAME for every GPT partition, read only, through
#                                        the loader's page (ufs=2 -> ufs_shared_pa=) or the kernel's own UFS host
#                                        controller driver (ufs_native=1); writes only with ufs_write=1, only into
#                                        ufs_part=NAME, only from the floor on (ufs_write_first= / the loader's)
#   iokit/bsddev/IOKitBSDInit.cpp        IOFindBSDRoot asks hakios_ufs_root() first: ufs_root=1 ufs_part=NAME roots
#                                        on /dev/ufs_NAME (rd=md0 stays on the command line; see README.md)
#   bsd/miscfs/apfs/apfs_vfsops.c        the real hakios_ufs_dev() (requests to the flash are worth putting together)
#
# hakios_ufs_init() is already chained (bsd/dev/hakios.input.c, after the root); the root path calls it earlier
# itself, and it runs once. Without the tag hakios.stubs.c keeps its no-op stubs (#ifndef HAKIOS_UFS) and nothing
# here is compiled in. Inert with the tag too unless the loader was asked for ufs=2.
# Run AFTER drivers/hakios-wire.py (which re-copies hakios.stubs.c / hakios.input.c from HakiOS).
# Idempotent; run in docker as root, then .work/run-rebuild.sh. XNU_WORK=<dir> points it at another tree.
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.environ.get("XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")


def edit(path, old, new, marker, tag):
    with open(path) as f:
        s = f.read()
    if marker in s:
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


# 1. the driver
for f in ("hakios.ufs.c", "hakios.ufshc.c", "hakios.ufshc.h"):
    shutil.copyfile(os.path.join(HERE, f), os.path.join(T, "bsd/dev", f))
print("  [ok]   copied bsd/dev/hakios.ufs.c, hakios.ufshc.c, hakios.ufshc.h")

# 2. bsd/conf/files: built only with the hakios_ufs tag
append_once(os.path.join(T, "bsd/conf/files"), "bsd/dev/hakios.ufs.c",
            "\n# HakiOS UFS flash (optional; ufs=2 to the loader, ufs_part=, ufs_root=1, ufs_native=1, ufs_write=1)\n"
            "bsd/dev/hakios.ufs.c\t\toptional hakios_ufs\n"
            "bsd/dev/hakios.ufshc.c\t\toptional hakios_ufs\n",
            "bsd/conf/files += hakios.ufs.c, hakios.ufshc.c")

# 3. config/MASTER: HAKIOS_UFS behind <hakios_ufs> (-DHAKIOS_UFS in every component's CONFIG_DEFINES, iokit too)
apfs_opt = "options\t\tAPFS\t\t# Apple File System (HakiOS built-in)\t# <apfs>\n"
edit(os.path.join(T, "config/MASTER"), apfs_opt,
     apfs_opt + "options\t\tHAKIOS_UFS\t# the phone's UFS flash (ufs=2, ufs_part=, ufs_root=1)\t# <hakios_ufs>\n",
     "HAKIOS_UFS", "config/MASTER options HAKIOS_UFS")

# 4. config/MASTER.arm64.bcm2837: the tag in BSD_DEV (the DEVELOPMENT config)
edit(os.path.join(T, "config/MASTER.arm64.bcm2837"), "BSD_EXTRAS_DEV ]", "hakios_ufs BSD_EXTRAS_DEV ]",
     " hakios_ufs ", "MASTER.arm64.bcm2837 BSD_DEV += hakios_ufs")

# 5. IOFindBSDRoot: ufs_root=1 before rd= (and before the md0 lookup, which panics on an md0 that is not there)
iok = os.path.join(T, "iokit/bsddev/IOKitBSDInit.cpp")
edit(iok,
     "kern_return_t\nIOFindBSDRoot( char * rootName, unsigned int rootNameSize,\n",
     "#if HAKIOS_UFS\n"
     "/* darwin-lisa: bsd/dev/hakios.ufs.c -- ufs_root=1: the root on /dev/ufs_NAME (ufs_part=NAME) */\n"
     "extern \"C\" int hakios_ufs_root(char *rootName, unsigned int rootNameSize, dev_t *root);\n"
     "#endif\n"
     "\n"
     "kern_return_t\nIOFindBSDRoot( char * rootName, unsigned int rootNameSize,\n",
     "extern \"C\" int hakios_ufs_root(", "IOKitBSDInit.cpp: hakios_ufs_root declaration")
edit(iok,
     "//\n//\tNow check if we are trying to root on a memory device\n//\n",
     "#if HAKIOS_UFS\n"
     "//\n"
     "//\tdarwin-lisa: ufs_root=1 ufs_part=NAME, the GPT partition NAME of the phone's flash (bsd/dev/hakios.ufs.c).\n"
     "//\tAsked before rd=, which stays rd=md0: bsd_init skips the root's seal check for a RAM disk only, and\n"
     "//\tlaunchd runs as a Restore environment because of it. Not asked for, or not there: rd= as before.\n"
     "//\n"
     "\tif (hakios_ufs_root(rootName, rootNameSize, root) == 0) {\n"
     "\t\tOSSafeReleaseNULL(matching);\n"
     "\t\t*oflags = 0;                                                                    /* Show that this is not network */\n"
     "\t\tgoto iofrootx;\n"
     "\t}\n"
     "#endif\n"
     "\n"
     "//\n//\tNow check if we are trying to root on a memory device\n//\n",
     "hakios_ufs_root(rootName, rootNameSize, root)", "IOKitBSDInit.cpp: IOFindBSDRoot ufs_root hook")

# 6. apfs: the real predicate (hakios.ufs.c with the tag, hakios.stubs.c's `false` without it)
edit(os.path.join(T, "bsd/miscfs/apfs/apfs_vfsops.c"),
     "static inline bool hakios_ufs_dev(dev_t dev) { (void)dev; return false; }\n",
     "bool hakios_ufs_dev(dev_t dev);  /* darwin-lisa: bsd/dev/hakios.ufs.c (hakios_ufs), else hakios.stubs.c: false */\n",
     "bool hakios_ufs_dev(dev_t dev);  /* darwin-lisa", "apfs_vfsops.c: hakios_ufs_dev from the driver")

# 7. the init call: already chained, checked only
with open(os.path.join(T, "bsd/dev/hakios.input.c")) as f:
    if f.read().count("\thakios_ufs_init();\n") != 1:
        print("  [FAIL] bsd/dev/hakios.input.c: hakios_ufs_init() is not chained (run drivers/hakios-wire.py first)")
        sys.exit(2)
print("  [ok]   hakios_ufs_init() chained from hakios_input_init() (and from IOFindBSDRoot with ufs_root=1)")

print("### UFS WIRE DONE")
