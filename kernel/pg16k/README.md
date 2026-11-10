# 16 KB pages (iOS-native granule) for the lisa kernel

`pg16k-wire.py` (verified on device, 2026-11-09): defines `__ARM_16K_PG__` for BCM2837
(ARM_PGSHIFT 14, TCR TG0/TG1 = 16 KB, T0SZ/T1SZ = 25; XNU's pmap/arm_vm_init were already
parameterised) and fixes `bsd/dev/memdev.c`, which hardcoded the 4 KB shift when rebuilding
md0 physical addresses (`pp<<12 | vaddr&4095` -> `PAGE_SHIFT`/`PAGE_MASK`). The 16 KB kernel
boots to userland, mounts the full root, and Apple's dyld runs. With 16 KB the dyld shared
cache's original 16 KB code signature is accepted as-is (no `resign-cache-4k.py`).

Note: `mach_loader.c` `pac_page_transform` stays at 4096 bytes even at 16 KB: the
apple-protect pager calls page_decrypt in 4 KB sub-chunks (vm_apple_protect.c).

`phase3-cachepac-wire.py` (STAGED, not yet proven): wraps each executable dyld-cache mapping
in the shared region with the same PAC-stripping apple-protect pager, since the cache is
mapped straight from the file and bypasses the exec'd-binary pac pager. Needs the cache to
actually map first (see docs/phase3-ondevice.md, WALL #2).
