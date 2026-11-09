#!/usr/bin/env python3
# darwin-lisa: wire HakiOS's miniamfi (AMFI stand-in) + kern_minimac (AMFI/Sandbox
# MACF policy stand-ins) into the xnu-12377 work tree, on top of the APFS + standard
# drivers port. So Apple's ad-hoc-signed arm64e userland runs and our unsigned code
# runs too. Idempotent. Run inside docker (root). Models hakios-wire.py.
import os, shutil, sys

S = "/home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/security"
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

# 1. sources: miniamfi.c + miniamfi_entitlements.cpp -> libkern/amfi/ (beside amfi.c);
#    kern_minimac.c -> bsd/kern/ (as in HakiOS).
copy(os.path.join(S, "miniamfi.c"), os.path.join(T, "libkern/amfi/miniamfi.c"), "libkern/amfi/miniamfi.c")
copy(os.path.join(S, "miniamfi_entitlements.cpp"), os.path.join(T, "libkern/amfi/miniamfi_entitlements.cpp"), "libkern/amfi/miniamfi_entitlements.cpp")
copy(os.path.join(S, "kern_minimac.c"), os.path.join(T, "bsd/kern/kern_minimac.c"), "bsd/kern/kern_minimac.c")

# 2. KPI rebase (xnu-8796 -> xnu-12377): kern_minimac.c allocates the argv strings with
#    kmem_alloc(..., KMA_DATA | KMA_ZERO, ...); the KMA_* flags moved out of <vm/vm_kern.h>
#    into the XNU-internal <vm/vm_kern_xnu.h> in 26 (same move hakios.audio.c needed).
edit(os.path.join(T, "bsd/kern/kern_minimac.c"),
     "#include <vm/vm_kern.h>\n",
     "#include <vm/vm_kern.h>\n#include <vm/vm_kern_xnu.h>   /* darwin-lisa: KMA_* flags on xnu-12377 */\n",
     "kern_minimac.c: include vm_kern_xnu.h")

# kern_minimac.c: vm_map_copy_overwrite() moved from <vm/vm_map.h> into the XNU-internal
# <vm/vm_map_xnu.h> in 26, and its signature gained the user-tagged address/size types plus
# an MTE-only sec_override arg. Match the in-tree caller (bsd/kern/kern_sysctl.c): include the
# header, cast to vm_map_address_t/vm_map_size_t, and guard the extra arg with #if HAS_MTE
# (off on this plain-arm64 SoC, so the 5-arg form is what compiles here).
edit(os.path.join(T, "bsd/kern/kern_minimac.c"),
     "#include <vm/vm_map.h>\n",
     "#include <vm/vm_map.h>\n#include <vm/vm_map_xnu.h>   /* darwin-lisa: vm_map_copy_overwrite on xnu-12377 */\n",
     "kern_minimac.c: include vm_map_xnu.h")
edit(os.path.join(T, "bsd/kern/kern_minimac.c"),
     "\tif (copy == NULL || vm_map_copy_overwrite(kernel_map, strings, copy, strings_size, FALSE) != KERN_SUCCESS) {\n",
     "\tif (copy == NULL || vm_map_copy_overwrite(kernel_map, (vm_map_address_t)strings, copy, (vm_map_size_t)strings_size,\n"
     "#if HAS_MTE\n"
     "\t    FALSE,\n"
     "#endif\n"
     "\t    FALSE) != KERN_SUCCESS) {\n",
     "kern_minimac.c: vm_map_copy_overwrite 26 signature")

# 3. libkern/conf/files: miniamfi.c + miniamfi_entitlements.cpp as standard, after amfi.c.
#    (HakiOS lists all three as standard; amfi.c is already present in this tree.)
edit(os.path.join(T, "libkern/conf/files"),
     "libkern/amfi/amfi.c \t\tstandard\n",
     "libkern/amfi/amfi.c \t\tstandard\n"
     "libkern/amfi/miniamfi.c \t\tstandard\n"
     "libkern/amfi/miniamfi_entitlements.cpp \tstandard\n",
     "libkern/conf/files += miniamfi")

# 4. bsd/conf/files: kern_minimac.c, built only with the MACF framework (as in HakiOS).
bf = os.path.join(T, "bsd/conf/files")
with open(bf) as f:
    bfc = f.read()
if "bsd/kern/kern_minimac.c" in bfc:
    print("  [skip] bsd/conf/files: kern_minimac already listed")
else:
    with open(bf, "a") as f:
        f.write("\n# HakiOS AMFI + Sandbox MACF policy stand-ins (ported from xnu-8796)\n"
                "bsd/kern/kern_minimac.c\t\t\toptional config_macf\n")
    print("  [ok]   bsd/conf/files += kern_minimac.c")

# 5. osfmk/kern/startup.c: declare + call minimac_policy_init() from kernel_bootstrap_thread,
#    inside CONFIG_MACF right after mac_policy_initmach()/vnguard (exactly where HakiOS calls it).
sc = os.path.join(T, "osfmk/kern/startup.c")
edit(sc,
     "extern void vnguard_policy_init(void);\n",
     "extern void vnguard_policy_init(void);\n"
     "extern void minimac_policy_init(void);   /* darwin-lisa: bsd/kern/kern_minimac.c */\n",
     "startup.c: minimac_policy_init extern")
edit(sc,
     "\tkernel_bootstrap_log(\"vnguard_policy_init\");\n"
     "\tvnguard_policy_init();\n"
     "#endif\n"
     "#endif\n",
     "\tkernel_bootstrap_log(\"vnguard_policy_init\");\n"
     "\tvnguard_policy_init();\n"
     "#endif\n"
     "\t/* darwin-lisa: stand-ins for the AMFI and Sandbox policies (kern_minimac.c) */\n"
     "\tkernel_bootstrap_log(\"minimac_policy_init\");\n"
     "\tminimac_policy_init();\n"
     "#endif\n",
     "startup.c: minimac_policy_init call")

print("### MINIAMFI WIRE DONE")
