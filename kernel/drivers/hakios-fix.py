#!/usr/bin/env python3
# darwin-lisa: KPI-rebase fixes for the HakiOS standard drivers on xnu-12377. Idempotent.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"

def edit(path, old, new, tag, optional=False):
    with open(path) as f:
        s = f.read()
    if new in s:
        print(f"  [skip] {tag}: already applied")
        return
    n = s.count(old)
    if n != 1:
        if optional:
            print(f"  [skip] {tag}: anchor found {n} times, skipping")
            return
        print(f"  [FAIL] {tag}: anchor found {n} times (need 1)")
        sys.exit(2)
    with open(path, "w") as f:
        f.write(s.replace(old, new))
    print(f"  [ok]   {tag}")

# hakios.audio.c: xnu-12377 moved kmem_alloc_contig + the KMA_* flags out of <vm/vm_kern.h>
# into the XNU-internal <vm/vm_kern_xnu.h> (same signature; cf. bsd/dev/unix_startup.c).
edit(T + "/bsd/dev/hakios.audio.c",
     "#include <vm/vm_kern.h>\n",
     "#include <vm/vm_kern.h>\n#include <vm/vm_kern_xnu.h>   /* darwin-lisa: kmem_alloc_contig + KMA_* on xnu-12377 */\n",
     "hakios.audio.c: include vm_kern_xnu.h")

# bsd/dev/memdev.c: port HakiOS's mdevgetkernelrange(), used by hakios.image.c and
# hakios.sparse.c to find a RAM disk's kernel-VA memory range. Not in stock XNU memdev.
# (size cast to uint64_t before the <<12 so a >4 GB RAM disk does not overflow.)
edit(T + "/bsd/dev/memdev.c",
     "dev_t           mdevlookup(int devid);\n",
     "dev_t           mdevlookup(int devid);\n"
     "int             mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);\n",
     "memdev.c: mdevgetkernelrange decl")
edit(T + "/bsd/dev/memdev.c",
     "\treturn mdev[devid].mdBDev;                                                      /* Return the device number */\n}\n",
     "\treturn mdev[devid].mdBDev;                                                      /* Return the device number */\n}\n"
     "\n"
     "/*\n"
     " * darwin-lisa: the kernel-VA memory range of a RAM-disk block device, by its dev_t\n"
     " * (HakiOS addition, for bsd/dev/hakios.image.c and hakios.sparse.c). A physical-memory\n"
     " * device has no kernel VA, so it returns ENOTSUP; an unknown device returns ENXIO.\n"
     " */\n"
     "int\n"
     "mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size)\n"
     "{\n"
     "\tfor (int i = 0; i < NB_MAX_MDEVICES; i++) {\n"
     "\t\tif ((mdev[i].mdFlags & mdInited) && mdev[i].mdBDev == dev) {\n"
     "\t\t\tif (mdev[i].mdFlags & mdPhys) {\n"
     "\t\t\t\treturn ENOTSUP;\n"
     "\t\t\t}\n"
     "\t\t\t*base = (vm_offset_t)(mdev[i].mdBase << 12);\n"
     "\t\t\t*size = (uint64_t)mdev[i].mdSize << 12;\n"
     "\t\t\treturn 0;\n"
     "\t\t}\n"
     "\t}\n"
     "\treturn ENXIO;\n"
     "}\n",
     "memdev.c: mdevgetkernelrange body")

print("### HAKIOS FIX DONE")
