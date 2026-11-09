#!/usr/bin/env python3
# darwin-lisa: wire HakiOS's Adreno 642L GPU/GMU bring-up driver (bsd/dev/hakios.adreno.c)
# into the xnu-12377 work tree as the OPTIONAL driver tag `hakios_adreno`. The standard input
# driver already chains hakios_adreno_init(); enabling the tag swaps the hakios.stubs.c no-op
# (#ifndef HAKIOS_ADRENO) for the real driver.
#
# The IOKit C++ "metal stack" that hakios.adreno.c calls into -- HakiosGPUStart/Stop (from
# iokit/Kernel/hakios.gpu.cpp) and MiniIOMobileFramebufferShow (from
# iokit/Kernel/MiniIOMobileFramebuffer.cpp) -- is NOT ported in this pass (see the report).
# Those three entry points are satisfied here by file-local no-op stubs in hakios.adreno_glue.c
# so the kernel-side driver compiles, links and registers. When the metal stack is ported, drop
# that glue file (its symbols would otherwise clash with the real ones). Idempotent; run in docker.
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

# 1. driver source.
copy(os.path.join(H, "bsd/dev/hakios.adreno.c"), os.path.join(T, "bsd/dev/hakios.adreno.c"), "bsd/dev/hakios.adreno.c")

# 1b. KPI rebase (xnu-8796 -> xnu-12377): the KMA_* flags moved out of <vm/vm_kern.h> into the
#     XNU-internal <vm/vm_kern_xnu.h> in 26 (same fix hakios.audio.c / kern_minimac.c needed);
#     adreno.c allocates GMU/GPU memory with kmem_alloc(..., KMA_KOBJECT|KMA_ZERO|KMA_DATA, ...).
edit(os.path.join(T, "bsd/dev/hakios.adreno.c"),
     "#include <vm/vm_kern.h>\n",
     "#include <vm/vm_kern.h>\n#include <vm/vm_kern_xnu.h>   /* darwin-lisa: KMA_* flags on xnu-12377 */\n",
     "hakios.adreno.c: include vm_kern_xnu.h")

# 2. metal-stack no-op glue (only while iokit/Kernel/hakios.gpu.cpp + MiniIOMobileFramebuffer.cpp
#    are not ported). Signatures match hakios.adreno.c's externs exactly.
GLUE = r'''/*
 * darwin-lisa: file-local no-op stand-ins for the HakiOS IOKit "metal stack" entry points that
 * bsd/dev/hakios.adreno.c calls but that are NOT yet ported to this xnu-12377 tree. The real ones:
 *
 *   HakiosGPUStart / HakiosGPUStop   iokit/Kernel/hakios.gpu.cpp  (the IOGPU/IOUserClient that
 *                                    starts and stops the GPU once hakios.adreno.c has brought the
 *                                    GMU up, and exposes it to userland Metal via HakiosMetal.bundle)
 *   MiniIOMobileFramebufferShow      iokit/Kernel/MiniIOMobileFramebuffer.cpp  (pushes a pixel
 *                                    buffer to the display controller)
 *
 * With these as no-ops the Adreno GMU bring-up driver compiles, links and registers, but the GPU
 * is never actually started and nothing is drawn. Porting the C++ metal stack is the next step
 * (see the report); when it lands, remove this file so its symbols do not clash with the real ones.
 */
#include <stdint.h>

void HakiosGPUStart(void);
void HakiosGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes);

void
HakiosGPUStart(void)
{
}

void
HakiosGPUStop(void)
{
}

void
MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes)
{
	(void)pixels;
	(void)width;
	(void)height;
	(void)row_bytes;
}
'''
glue_path = os.path.join(T, "bsd/dev/hakios.adreno_glue.c")
with open(glue_path, "w") as f:
    f.write(GLUE)
print("  [ok]   wrote bsd/dev/hakios.adreno_glue.c (metal-stack stubs)")

# 3. bsd/conf/files: the optional driver + its metal-stack glue.
bf = os.path.join(T, "bsd/conf/files")
with open(bf) as f:
    bfc = f.read()
if "bsd/dev/hakios.adreno.c" in bfc:
    print("  [skip] bsd/conf/files: hakios.adreno already listed")
else:
    with open(bf, "a") as f:
        f.write("\n# HakiOS Adreno 642L GPU/GMU bring-up driver (optional; ported from xnu-8796).\n"
                "# hakios.adreno_glue.c carries no-op stand-ins for the not-yet-ported IOKit metal stack.\n"
                "bsd/dev/hakios.adreno.c\t\t\toptional hakios_adreno\n"
                "bsd/dev/hakios.adreno_glue.c\t\toptional hakios_adreno\n")
    print("  [ok]   bsd/conf/files += hakios.adreno.c + glue")

# 4. config/MASTER: declare the HAKIOS_ADRENO option behind the <hakios_adreno> tag.
edit(os.path.join(T, "config/MASTER"),
     "options\t\tHAKIOS_GOODIX\t# Goodix/FocalTech Berlin touch\t# <hakios_goodix>\n",
     "options\t\tHAKIOS_GOODIX\t# Goodix/FocalTech Berlin touch\t# <hakios_goodix>\n"
     "options\t\tHAKIOS_ADRENO\t# Adreno 642L GPU/GMU bring-up\t# <hakios_adreno>\n",
     "config/MASTER options HAKIOS_ADRENO")

# 5. config/MASTER.arm64.bcm2837: enable the hakios_adreno tag in DEVELOPMENT (via BSD_DEV).
edit(os.path.join(T, "config/MASTER.arm64.bcm2837"),
     "#  BSD_DEV =        [ BSD_BASE config_netboot config_imgsrc_access pgo config_vnguard rosetta hakios_goodix BSD_EXTRAS_DEV ]",
     "#  BSD_DEV =        [ BSD_BASE config_netboot config_imgsrc_access pgo config_vnguard rosetta hakios_goodix hakios_adreno BSD_EXTRAS_DEV ]",
     "MASTER.arm64.bcm2837 BSD_DEV += hakios_adreno")

print("### ADRENO WIRE DONE")
