# Phase 3 prelink — experimental enablers for the plain-arm64 (no-PAC) kernel

The UniXNU prelink (`kits/XnuPrelink/emit_prelink.py`) targets arm64e kernelcaches. To run it
against our plain-arm64 source kernel these changes are needed (see ../../docs/phase3-ondevice.md
for the full blocker chain). They live in the build work tree / submodule, not upstream — keep them here.

## 1. Kernel link flags (makedefs/MakeInc.def)
Plain arm64 defines no `LDFLAGS_KERNEL_ONLY_SUBARCH_arm64`. Add, so the native kernel carries the
relocation info the prelinker needs (split-seg + chained fixups):

    LDFLAGS_KERNEL_ONLY_SUBARCH_arm64 = \
        -Wl,-add_split_seg_info \
        -Wl,-fixup_chains \
        -Wl,-kernel

Build with `XNU_KERNEL_CHAINED_FIXUPS=1`, and delete `mach.development.bcm2837.unstripped*` to force a
relink. Produces `LC_SEGMENT_SPLIT_INFO` + `LC_DYLD_CHAINED_FIXUPS` (format 6, 64_OFFSET).
NOTE: such a kernel is kernelcache-targeted and no longer boots standalone — rebuild WITHOUT these for
on-device bring-up (kernel26.nopac).

## 2. Prelink format-6 support (kc_image.py) — kc_image-format6.patch
The kit only parsed chained-pointer formats 7 (ARM64E_KERNEL) and 8 (64_KERNEL_CACHE). Plain arm64
`-fixup_chains` emits format 6 (DYLD_CHAINED_PTR_64_OFFSET). The patch adds format-6 parsing.

## 3. loader buffer shrink (kernel/loader-console/loader_console.c) — already committed
LOADER_LOG_SIZE 1536 KiB -> 256 KiB, so our debug buffers fit the kernelcache __DATA slot.

## Remaining blocker (NOT mechanical): symbol matching
`emit_prelink.py` reaches `write_candidate` then fails `Incomplete pairing: 53075 unresolved references`
(names only 544/3094 kext-facing targets by heuristics). Needs `--fileset-reference` (a symbol-bearing
xnu-12377 kernel) and/or a RELEASE build closer to Apple's, plus the apple-oss reference sources.
