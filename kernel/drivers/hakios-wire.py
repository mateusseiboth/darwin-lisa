#!/usr/bin/env python3
# darwin-lisa: wire HakiOS's "standard" built-in bsd/dev drivers into the xnu-12377
# work tree, on top of the existing APFS port. Idempotent. Run inside docker (root).
import os, shutil, sys

H = "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/xnu-src/xnu-8796.121.2"
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"

# the standard (always-built) HakiOS drivers that are relevant to the bring-up boot path
DRIVERS = [
    "hakios.version.c",
    "hakios.platform.c",
    "hakios.stubs.c",
    "hakios.input.c",
    "hakios.net.c",
    "hakios.image.c",
    "hakios.sparse.c",
    "hakios.audio.c",
]
DEV_HEADERS = ["hakios.drivers.h"]
SYS_HEADERS = [
    "hakios.version.h",
    "hakios.input.h",
    "hakios.net.h",
    "hakios.sparse.h",
    "hakios.virtio_blk.h",
    "hakios.audio.h",
]

def copy(src, dst, tag):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    print(f"  [ok]   copied {tag}")

def edit(path, old, new, tag):
    with open(path) as f:
        s = f.read()
    if new in s and old not in s:
        print(f"  [skip] {tag}: already applied")
        return
    if new in s:
        # new present AND old present: the anchor is the literal substring of new; already applied
        print(f"  [skip] {tag}: marker already present")
        return
    n = s.count(old)
    if n != 1:
        print(f"  [FAIL] {tag}: anchor found {n} times (need 1) in {path}")
        sys.exit(2)
    with open(path, "w") as f:
        f.write(s.replace(old, new))
    print(f"  [ok]   {tag}")

# 1. driver sources + headers
for f in DRIVERS:
    copy(os.path.join(H, "bsd/dev", f), os.path.join(T, "bsd/dev", f), "bsd/dev/" + f)
for f in DEV_HEADERS:
    copy(os.path.join(H, "bsd/dev", f), os.path.join(T, "bsd/dev", f), "bsd/dev/" + f)
for f in SYS_HEADERS:
    copy(os.path.join(H, "bsd/sys", f), os.path.join(T, "bsd/sys", f), "bsd/sys/" + f)

# 2. the loader-symbol glue (file-local no-op stubs for HakiOS loader symbols this tree
#    does not export the same way). loader_return() is NOT here: loader_console.c has it.
GLUE = r'''/*
 * darwin-lisa: stand-ins for the few HakiOS xnuloader symbols that the standard bsd/dev
 * drivers reference but that this xnu-12377 bring-up tree does not provide the same way.
 * Each is non-functional for boot; they only let the drivers link.
 *
 *   loader_log_line  - HakiOS's xnuloader live-log line, used by dev/hakios.input.c's touch
 *                      latency tracing (boot-arg touch_lat=1). No live loader-log channel of
 *                      that shape here, so this is a no-op.
 *   loader_sound     - HakiOS's { phys, bytes, kva } of the audio recording the loader ships
 *                      to the host (dev/hakios.audio.c, boot-arg rec_sound). The tree's
 *                      osfmk/arm64/loader_console.c keeps its OWN static loader_sound[3]; this
 *                      definition only satisfies the driver's extern, it is not read by the
 *                      loader, so on-host sound capture is inert.
 *
 * loader_return() is intentionally absent: osfmk/arm64/loader_console.c already provides it.
 */
#include <stdint.h>

void loader_log_line(const char *line);
void
loader_log_line(const char *line)
{
	(void)line;
}

uint64_t loader_sound[3];
'''
glue_path = os.path.join(T, "bsd/dev/hakios.lisa_glue.c")
with open(glue_path, "w") as f:
    f.write(GLUE)
print("  [ok]   wrote bsd/dev/hakios.lisa_glue.c")

# 3. bsd/conf/files : the standard drivers + the glue
files = os.path.join(T, "bsd/conf/files")
block = (
    "\n# HakiOS standard built-in bring-up drivers (ported from xnu-8796), + loader glue\n"
    "bsd/dev/hakios.version.c\t\tstandard\n"
    "bsd/dev/hakios.platform.c\t\tstandard\n"
    "bsd/dev/hakios.stubs.c\t\tstandard\n"
    "bsd/dev/hakios.input.c\t\tstandard\n"
    "bsd/dev/hakios.net.c\t\tstandard\n"
    "bsd/dev/hakios.image.c\t\tstandard\n"
    "bsd/dev/hakios.sparse.c\t\tstandard\n"
    "bsd/dev/hakios.audio.c\t\tstandard\n"
    "bsd/dev/hakios.lisa_glue.c\t\tstandard\n"
)
with open(files) as f:
    fc = f.read()
if "bsd/dev/hakios.version.c" in fc:
    print("  [skip] bsd/conf/files: hakios drivers already listed")
else:
    with open(files, "a") as f:
        f.write(block)
    print("  [ok]   bsd/conf/files += hakios standard drivers")

# 4. bsd/kern/bsd_init.c : includes + init calls
bi = os.path.join(T, "bsd/kern/bsd_init.c")
edit(bi,
     "#include <miscfs/devfs/devfsdefs.h>     /* for devfs_kernel_mount() */\n",
     "#include <miscfs/devfs/devfsdefs.h>     /* for devfs_kernel_mount() */\n"
     "/* darwin-lisa: HakiOS standard built-in drivers */\n"
     "#include <sys/hakios.version.h>\n"
     "#include <sys/hakios.input.h>\n"
     "#include <sys/hakios.audio.h>\n"
     "#include <sys/hakios.sparse.h>\n",
     "bsd_init.c hakios includes")
edit(bi,
     "\t\tdevfs_kernel_mount(mounthere);\n",
     "\t\tdevfs_kernel_mount(mounthere);\n"
     "\t\t/* darwin-lisa: HakiOS standard built-in drivers (RAM-disk root path) */\n"
     "\t\thakios_version_init();         /* kern.hakios.version, and the version in the boot log */\n"
     "\t\thakios_input_init();           /* /dev/hakios-touch (chains the optional input drivers, all stubbed) */\n"
     "\t\thakios_audio_init();           /* /dev/hakios-audio (chains the optional audio drivers, all stubbed) */\n"
     "\t\thakios_sparse_init();          /* /dev/rsp0, part of a root image too big to hold (boot-arg apfs_sparse) */\n",
     "bsd_init.c hakios init calls")

print("### HAKIOS WIRE DONE")
