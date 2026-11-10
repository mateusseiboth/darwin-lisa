# Caminho B: HakiOS standard platform/stub drivers ported to xnu-12377

The `standard` (always-built) HakiOS bsd/dev drivers, ported from xnu-8796 so our plain-arm64 kernel
reproduces HakiOS's /dev population + boot init order (a prerequisite for the HakiOS-style RAM-disk root
mount). The kernel BUILDS/links clean with these on top of the APFS port (22.41 -> 22.44 MB).

- `hakios.{version,platform,stubs,input,net,image,sparse,audio}.c` + `hakios.drivers.h` + `sys/hakios.*.h`
  — the drivers. `hakios.platform.c` self-registers via STARTUP(EARLY_BOOT) (a boot banner + shared SPMI/
  SMMU locks + the kern.hakios sysctl); the rest are bsd init functions chained from bsd_init. All the
  OPTIONAL drivers (goodix touch, adreno GPU, wifi, audio DSPs, modem...) resolve to the no-op guards in
  `hakios.stubs.c` since none are built yet. `hakios.image.c`/`hakios.sparse.c` are the RAM-disk file readers.
- `hakios.lisa_glue.c` — file-local no-op stubs for HakiOS loader symbols absent here (loader_log_line,
  loader_sound); non-functional for boot. (loader_return is the real one from loader_console.c.)
- `hakios-wire.py` — idempotent wiring of a fetched xnu-work tree: the 9 `standard` lines in bsd/conf/files,
  4 includes + 4 init calls in bsd/kern/bsd_init.c (after devfs_kernel_mount), the kmem_alloc_contig include
  fix in hakios.audio.c.
- `hakios-fix.py` — the KPI rebase: `#include <vm/vm_kern_xnu.h>` in hakios.audio.c (KMA_* flags moved there
  in 26) and the `mdevgetkernelrange` port into memdev.c (byte-identical mdev[] struct; one uint32 shift
  cast fix). Only two KPI fixes were needed — the cdevsw/bdevsw/ifnet/mbuf/devfs KPIs are stable 16->26.

GPU (Adreno) + touch (Goodix) are the end-goal (as on iOS 16) but are OPTIONAL drivers that come after the
kernel boots to userland.

`bootargs/lisa-defaults-wire.py` (pexpert/gen/bootargs.c): with `lisa=1` on the line, the phone's hardware boot-args
(touch, processor clock, panel geometry, Wi-Fi interrupt mode and address) come from a list built into the kernel when
the line does not name them; the line always wins, and without `lisa=1` nothing changes. Host test:
`bootargs/test/run-tests.sh`. Why: the loader keeps 607 characters of the line (`docs/evening-session.md`).
