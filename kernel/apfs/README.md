# Caminho B: HakiOS APFS driver ported to xnu-12377 (iOS 26)

The abandoned prelink path (linking Apple's binary kexts) is replaced by HakiOS's proven approach:
a standalone kernel with HakiOS's own built-in APFS driver mounts an in-RAM APFS image as root.
This directory is the HakiOS APFS driver (from xnu-8796) ported into our plain-arm64 xnu-12377 kernel.
It BUILDS and links clean (kernel 21.3 MB -> 22.4 MB; symbols _apfs_mountroot/_apfs_vfsops/... present;
`options APFS` on). The 15K-line driver compiled essentially unchanged — the VFS/vnode/buf KPI surface
is stable iOS 16 -> iOS 26.

## Files
- `*.c` / `*.h` / `libdeflate/` / `libzbitmap/` — the driver source (apfs_vfsops, apfs_vnops, apfs_lib,
  apfs_store, apfs_write, apfs_deflate, apfs_zbitmap + decompression libs). Read-write APFS.
- `apfs-wire.py` — idempotent wiring of a fetched xnu-work tree: appends the `bsd/miscfs/apfs/*.c optional apfs`
  lines to bsd/conf/files, the `apfs_deflate.o_CFLAGS_ADD` rule to bsd/conf/Makefile.arm64, `options APFS` to
  config/MASTER, the `apfs` tag to FILESYS_BASE in config/MASTER.arm64.bcm2837, and the apfs vfstable +
  vnodeop registration to bsd/vfs/vfs_conf.c.
- `apfs-fix1.py` — the one KPI rebase fix: file-local no-op for `thread_runnable_and_run_self` (a HakiOS
  diagnostic for lock-wait histograms; reads per-thread recount internals that changed 16->26; zero effect
  on mount/read). Also two HakiOS-platform `#include`s in apfs_vfsops.c replaced by file-local `false` stubs
  (the am_nx.cheap_requests optimization; correct for a RAM disk) and the cosmetic kern.hakios_memory sysctl
  `#if 0`'d. No mount/read logic removed; the full read-write path is compiled in.
- `apfs-build.sh` — build it in the toolchain container.

## State / next
The kernel builds + boots, but stalls in IOKit at `IOServicePH ... did 0xf->0xf` BEFORE reaching
bsd_init's vfs_mountroot, so the apfs mount is not yet exercised on device. The loader already hands the
ramdisk over (`ramdisk=` payload -> a `RAMDisk` memory-map DT node -> the stock imageboot/di_root path ->
md0, with `rd=md0` auto-added to the cmdline). Getting past IOServicePH needs HakiOS's core platform/stub
drivers (hakios.platform.c/stubs.c/...) and/or resolving the single-wake-source idle stall. A matching
read-verified iOS-26 test root (static no-PAC /sbin/launchd) is at ~/Dev/mobile/userland-26/out/root-26.apfs.

## readdir (2026-11-11)

`apfs_vnop_readdir` serves both dirent formats (`VNODE_READDIR_EXTENDED` gives `struct direntry`, with `d_seekoff`
for `VNODE_READDIR_REQSEEKOFF`) and uses the entry's INDEX as the directory cookie. Before, it served 32-bit dirents
only with a byte offset into that stream, and every 64-bit enumeration (`getdirentries64`, `getattrlistbulk`:
NSFileManager, LaunchServices' scan of `/Applications`) went through `vnode_readdir64()`'s conversion and stopped
short: lsd saw 25 of 252 applications, 32 of 272 frameworks, 161 of 2767 private frameworks, and the home screen had
three icons (the host `apfs-tool ls` lists them all, so the walk itself was fine). `apfs-wire.py` also flags the
vfsconf entry `VFC_VFSREADDIR_EXTENDED`, so VFS calls the vnop directly. Kernel: `kernel-16k-apfs-readdir.bak`.
