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
# and it adds boot-arg metal_plugin=NAME to the framebuffer's accelerator (step 6), inert without metal=1.
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

# 6. boot-arg metal_plugin=NAME: which bundle the accelerator names with metal=1 (default HakiosMetal, the window
#    server's plug-in). metal_plugin=HakiosMetalProbe names the discovery plug-in, which shows the device to metal_test
#    alone, so the window server stays in software while Metal itself is tested. Same class name in both bundles.
edit(fb,
     '\t\t\taccelerator->setProperty("MetalPluginName", "HakiosMetal");\n'
     '\t\t\taccelerator->setProperty("MetalPluginClassName", "HakiosMetalDevice");\n'
     '\t\t\tIOLog("MiniIOMobileFramebuffer: metal=1: the accelerator names HakiosMetal as its Metal plug-in\\n");\n',
     '\t\t\tchar plugin[64] = "HakiosMetal";\n\n'
     '\t\t\t/* darwin-lisa (gpu-wire.py): boot-arg metal_plugin=NAME names another bundle (HakiosMetalProbe) */\n'
     '\t\t\tPE_parse_boot_arg_str("metal_plugin", plugin, sizeof(plugin));\n'
     '\t\t\taccelerator->setProperty("MetalPluginName", plugin);\n'
     '\t\t\taccelerator->setProperty("MetalPluginClassName", "HakiosMetalDevice");\n'
     '\t\t\tIOLog("MiniIOMobileFramebuffer: metal=1: the accelerator names %s as its Metal plug-in\\n", plugin);\n',
     "MiniIOMobileFramebuffer.cpp: boot-arg metal_plugin=")

# 7. the GPU's firmware built into the kernel (bsd/dev/hakios.adreno_fw.h, generated here, never in git: the blobs are
#    Qualcomm's). The root is on the flash now and hakios_image_file() reads only the RAM disks md0/md1, so without
#    this gpu=1 needs a firmware RAM disk uploaded at every run. image_file() still asks the RAM disks first: a firmware
#    RAM disk (the pmOS set, say) overrides the built-in one. FW_GPU=DIR picks the set (default the phone's stock set,
#    which brought the GPU up on HakiOS 16.5); FW_GPU= (empty) leaves the firmware out.
FW = os.environ.get("FW_GPU", "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/fw-gpu")
FILES = ("a660_gmu.bin", "a660_sqe.fw", "a660_zap.mdt", "a660_zap.b02")
header = os.path.join(T, "bsd/dev/hakios.adreno_fw.h")
adreno = os.path.join(T, "bsd/dev/hakios.adreno.c")
if FW and all(os.path.exists(os.path.join(FW, f)) for f in FILES):
    import hashlib
    out = ["/* darwin-lisa: generated by kernel/drivers/gpu/gpu-wire.py from " + FW + ". Qualcomm firmware: never commit. */",
           "#define HAKIOS_ADRENO_BUILTIN_FW 1"]
    rows = []
    for i, f in enumerate(FILES):
        data = open(os.path.join(FW, f), "rb").read()
        out.append("static const uint8_t adreno_fw_%d[%d] __attribute__((aligned(64))) = {" % (i, len(data)))
        for at in range(0, len(data), 24):
            out.append("\t" + ", ".join("0x%02x" % b for b in data[at:at + 24]) + ",")
        out.append("};")
        rows.append('\t{ "%s", adreno_fw_%d, %d },  /* sha256 %s */' % (f, i, len(data), hashlib.sha256(data).hexdigest()[:16]))
    out.append("static const struct { const char *name; const uint8_t *data; uint64_t size; } adreno_builtin_fw[] = {")
    out += rows
    out.append("};")
    text = "\n".join(out) + "\n"
    if os.path.exists(header) and open(header).read() == text:
        print("  [skip] bsd/dev/hakios.adreno_fw.h: the same firmware already")
    else:
        with open(header, "w") as f:
            f.write(text)
        print(f"  [ok]   bsd/dev/hakios.adreno_fw.h: {', '.join(FILES)} from {FW}")
    edit(adreno,
         "\tif (!image_asked_quietly) {\n"
         "\t\tprintf(\"adreno: %u: no %s/%s in the small image: put the phone's /vendor/firmware/%s into fw-gpu/ and run \"\n",
         "#if __has_include(\"hakios.adreno_fw.h\")\n"
         "\t/* darwin-lisa (gpu-wire.py): the set built into the kernel, when no RAM disk has the file */\n"
         "\tfor (unsigned i = 0; strcmp(directory, \"fw\") == 0 && i < sizeof(adreno_builtin_fw) / sizeof(adreno_builtin_fw[0]); i++) {\n"
         "\t\tif (strcmp(name, adreno_builtin_fw[i].name) == 0) {\n"
         "\t\t\t*size = adreno_builtin_fw[i].size;\n"
         "\t\t\tif (!image_asked_quietly) {\n"
         "\t\t\t\tprintf(\"adreno: %u: %s/%s: %llu bytes built into the kernel\\n\", step, directory, name, *size);\n"
         "\t\t\t}\n"
         "\t\t\treturn adreno_builtin_fw[i].data;\n"
         "\t\t}\n"
         "\t}\n"
         "#endif\n"
         "\tif (!image_asked_quietly) {\n"
         "\t\tprintf(\"adreno: %u: no %s/%s in the small image: put the phone's /vendor/firmware/%s into fw-gpu/ and run \"\n",
         "hakios.adreno.c: image_file() falls back to the built-in firmware")
    edit(adreno,
         "static bool image_asked_quietly;        /* image_file() says nothing: is the file there at all */\n",
         "static bool image_asked_quietly;        /* image_file() says nothing: is the file there at all */\n"
         "#if __has_include(\"hakios.adreno_fw.h\")\n#include \"hakios.adreno_fw.h\"      /* darwin-lisa: gpu-wire.py */\n#endif\n",
         "hakios.adreno.c: includes hakios.adreno_fw.h when it is there")
else:
    print("  [skip] no firmware built in (FW_GPU empty or incomplete): gpu=1 needs a firmware RAM disk")

print("### GPU WIRE DONE")
