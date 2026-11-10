#!/usr/bin/env python3
# darwin-lisa: wire HakiosGPU (HakiOS's drivers/gpu/hakios.gpu.cpp, ported to xnu-12377 and vendored next to this
# script) into the xnu-work tree as the OPTIONAL tag `hakios_gpu`. It is the IOKit user client through which userland
# drives the Adreno 642L once hakios.adreno.c has brought it up (boot-arg gpu=1): the msm DRM requests Mesa's freedreno,
# Turnip (through tools/mesa/hakios-drm) and hakios-metal speak, plus IOSurfaces as GPU buffers.
#
#   iokit/Kernel/hakios.gpu.cpp     HakiosGPU + HakiosGPUUserClient, HakiosGPUStart/Stop, the real MiniIOSurfaceDrawn()
#
# Needs the tags hakios_adreno (adreno-wire.py: the hakios_gpu_* functions) and hakios_display (display/display-wire.py:
# MiniIOSurface, MiniIOMobileFramebufferShow). Two no-op stand-ins go under #ifndef HAKIOS_GPU so that a kernel without
# the tag builds as before:
#   bsd/dev/hakios.adreno_glue.c          HakiosGPUStart / HakiosGPUStop
#   iokit/Kernel/MiniIOMobileFramebuffer.cpp   MiniIOSurfaceDrawn
# ORDER on a fresh tree: ../adreno-wire.py, ../display/display-wire.py, then this one LAST. Both of those rewrite the
# file this script guards (adreno-wire.py writes the whole glue file, display-wire.py copies MiniIOMobileFramebuffer.cpp
# over), so running either of them again means running this one again too, or the kernel links two copies.
# Inert at run time unless boot-arg gpu=1 starts the GPU (hakios_adreno_init -> HakiosGPUStart()).
# Idempotent. XNU_WORK=<dir> points it at another tree (a dry run on a copy). Run in docker as root, then the rebuild.
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = os.environ.get("XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")


def read(path):
    with open(path) as f:
        return f.read()


def edit(path, old, new, tag):
    s = read(path)
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
    if marker in read(path):
        print(f"  [skip] {tag}: already applied")
        return
    with open(path, "a") as f:
        f.write(text)
    print(f"  [ok]   {tag}")


def require(path, needle, why):
    if not os.path.exists(path) or needle not in read(path):
        print(f"  [FAIL] {why}: {needle!r} not in {path}")
        sys.exit(2)


# 0. what this builds on
require(os.path.join(T, "bsd/conf/files"), "bsd/dev/hakios.adreno.c", "run ../adreno-wire.py first")
require(os.path.join(T, "iokit/conf/files"), "iokit/Kernel/MiniIOSurface.cpp", "run ../display/display-wire.py first")
require(os.path.join(T, "iokit/Kernel/MiniIOSurface.h"), "uint32_t                        drawn;", "MiniIOSurface without the GPU's fence")

# 1. the source
shutil.copyfile(os.path.join(HERE, "hakios.gpu.cpp"), os.path.join(T, "iokit/Kernel/hakios.gpu.cpp"))
print("  [ok]   copied iokit/Kernel/hakios.gpu.cpp")

# 2. iokit/conf/files
append_once(os.path.join(T, "iokit/conf/files"), "iokit/Kernel/hakios.gpu.cpp",
            "\n# HakiosGPU: the Adreno 642L's user client (msm DRM requests + IOSurfaces as buffers; boot-arg gpu=1).\n"
            "iokit/Kernel/hakios.gpu.cpp\t\toptional hakios_gpu iokitcpp\n",
            "iokit/conf/files += hakios.gpu.cpp")

# 3. config: the option and the tag (after hakios_display, whose MiniIOSurface it uses)
edit(os.path.join(T, "config/MASTER"),
     "options\t\tHAKIOS_DISPLAY\t# IOSurface + IOMobileFramebuffer (display=1)\t# <hakios_display>\n",
     "options\t\tHAKIOS_DISPLAY\t# IOSurface + IOMobileFramebuffer (display=1)\t# <hakios_display>\n"
     "options\t\tHAKIOS_GPU\t# HakiosGPU user client for the Adreno (gpu=1)\t# <hakios_gpu>\n",
     "config/MASTER options HAKIOS_GPU")
m = os.path.join(T, "config/MASTER.arm64.bcm2837")
if " hakios_gpu " in read(m):
    print("  [skip] MASTER.arm64.bcm2837 BSD_DEV += hakios_gpu: already applied")
else:
    edit(m, " hakios_display ", " hakios_display hakios_gpu ", "MASTER.arm64.bcm2837 BSD_DEV += hakios_gpu")

# 4. the adreno glue's no-op HakiosGPUStart/HakiosGPUStop only without the tag
glue = os.path.join(T, "bsd/dev/hakios.adreno_glue.c")
STUBS = "void\nHakiosGPUStart(void)\n{\n}\n\nvoid\nHakiosGPUStop(void)\n{\n}\n"
if os.path.exists(glue):
    edit(glue, STUBS,
         "#ifndef HAKIOS_GPU      /* darwin-lisa: the real ones are iokit/Kernel/hakios.gpu.cpp (gpu-wire.py) */\n"
         + STUBS + "#endif\n",
         "hakios.adreno_glue.c: HakiosGPUStart/Stop under #ifndef HAKIOS_GPU")
else:
    print("  [skip] no bsd/dev/hakios.adreno_glue.c")

# 5. the framebuffer's no-op MiniIOSurfaceDrawn only without the tag
fb = os.path.join(T, "iokit/Kernel/MiniIOMobileFramebuffer.cpp")
DRAWN = ("bool\nMiniIOSurfaceDrawn(__unused MiniIOSurface * surface, __unused uint64_t milliseconds)\n"
         "{\n\treturn true;\n}\n")
edit(fb, DRAWN,
     "#ifndef HAKIOS_GPU      /* darwin-lisa: with the GPU the real one is iokit/Kernel/hakios.gpu.cpp (gpu-wire.py) */\n"
     + DRAWN + "#endif\n",
     "MiniIOMobileFramebuffer.cpp: MiniIOSurfaceDrawn under #ifndef HAKIOS_GPU")

print("### GPU WIRE DONE")
