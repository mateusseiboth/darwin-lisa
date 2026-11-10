#!/usr/bin/env python3
# darwin-lisa: switch the lisa BCM2837 DEVELOPMENT kernel from a 4 KB to a 16 KB
# translation granule, and fix the one boot-path driver that hardcoded the 4 KB
# page shift (the resident md0 RAM-disk strategy routine).
#
# XNU's own pmap / TCR / arm_vm_init are already parameterised on __ARM_16K_PG__
# (H13-H17 + VMAPPLE define it; only BCM2837 was left at 4 KB), and the ARM64
# kernel is always linked with 16 KB segment alignment (makedefs/MakeInc.def),
# so defining the knob flips the whole kernel to a 16 KB granule:
#   ARM_PGSHIFT 14, ARM_PGBYTES 0x4000, TCR TG0/TG1 = 16 KB, T0SZ/T1SZ = 25.
#
# memdev.c reconstructs a physical address from pmap_find_phys(), which returns a
# page-frame number in PAGE_SIZE units. At 16 KB that frame is a 16 KB frame, so
# the hardcoded `pp << 12 | vaddr & 4095` under-shifts and truncates the offset.
# PAGE_SHIFT / PAGE_MASK is correct for both granules (reverts cleanly to 4 KB).
#
# Idempotent. Run inside docker (root), like the other *-wire.py scripts.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"


def edit(path, old, new, tag, count=1):
    with open(path, "r") as f:
        data = f.read()
    if new in data and old not in data:
        print(f"  = {tag}: already applied")
        return
    n = data.count(old)
    if n == 0:
        print(f"  ! {tag}: ANCHOR NOT FOUND -> {old!r}")
        sys.exit(1)
    if n != count:
        print(f"  ! {tag}: expected {count} occurrence(s), found {n}")
        sys.exit(1)
    data = data.replace(old, new)
    with open(path, "w") as f:
        f.write(data)
    print(f"  + {tag}: applied ({count}x)")


bcm = f"{T}/pexpert/pexpert/arm64/BCM2837.h"
edit(
    bcm,
    "#define __ARM64_PMAP_SUBPAGE_L1__ 1\n",
    "#define __ARM64_PMAP_SUBPAGE_L1__ 1\n\n"
    "/* darwin-lisa: run the lisa kernel on a 16 KB translation granule, like a\n"
    " * real iOS device (the dyld shared region is mapped 16 KB-aligned). XNU's\n"
    " * pmap/TCR/arm_vm_init already key off this; the ARM64 link is 16 KB-aligned. */\n"
    "#define __ARM_16K_PG__            1\n",
    "BCM2837.h: define __ARM_16K_PG__",
)

mem = f"{T}/bsd/dev/memdev.c"
edit(
    mem,
    "((addr64_t)pp << 12) | (addr64_t)(vaddr & 4095)",
    "((addr64_t)pp << PAGE_SHIFT) | (addr64_t)(vaddr & PAGE_MASK)",
    "memdev.c: md strategy phys reconstruct (read+write)",
    count=2,
)
edit(
    mem,
    "mapping_set_mod((ppnum_t)(paddr >> 12))",
    "mapping_set_mod((ppnum_t)(paddr >> PAGE_SHIFT))",
    "memdev.c: md strategy mapping_set_mod frame",
    count=1,
)

print("pg16k-wire: done")
