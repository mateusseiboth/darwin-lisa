# Caminho B: the 5 bring-up fixes that carry the kernel to userland (/sbin/launchd, pid 1)

With the APFS driver + platform drivers + these 5 fixes, our xnu-12377 plain-arm64 kernel boots on lisa
through bsd_init, mounts the in-RAM APFS root (md0), and execs /sbin/launchd as pid 1 in EL0 userland.
Confirmed on device (bootcpu=7, cpus=1, 0 panics):
  `BSD root: md0` -> `apfs: volume 0 "System" ...` -> `==== iOS 26.5.2 (23F84) userland on lisa: /sbin/launchd alive ==== init26: pid=1`.

Each fix guards a dependency that is absent on this minimal kernel (no Apple kexts, no CPU-interrupt-controller
IOKit driver, no RTC/CTRR/IOMedia/AMFI) instead of hanging or asserting. None change the CPU count or the vtimer.
The `.diff` files here are the raw work-tree-vs-pristine capture (they also contain debug `DLISA-*` printf markers,
and machine_routines.c.diff additionally contains the already-committed virtual-timer + decrementer-floor patches).

1. **The primary stall** — `osfmk/kern/startup.c`, before `#if KPERF` (~line 916): `kperf_init_early()` ->
   `ml_wait_max_cpus()` sleeps on `max_cpus_initialized` forever, because `ml_set_max_cpus()` is only called from
   `AppleARMSMP.cpp::cpu_boot_thread` (under `#if USE_APPLEARMSMP`, off for BCM2837) and
   `IOCPUInterruptController::initCPUInterruptController` (no caller in-tree — we drive the GIC at pexpert level).
   Fix: `#if !USE_APPLEARMSMP\n  ml_set_max_cpus(machine_info.max_cpus);\n#endif`. The arg is __unused and
   machine_info.max_cpus is already set, so this is pure signal (no CPU-count change).
2. **IORTC wait** — `iokit/Kernel/IOStartIOKit.cpp` `IOKitInitializeTime()`: drop the
   `waitForService(resourceMatching("IORTC"), &t)` (no RTC driver; the deadline-sleep never wakes on cpus=1) and
   call `clock_initialize_calendar()` directly.
3. **CTRR assert** — `osfmk/arm64/machine_routines.c` `ml_unsafe_kernel_text_init()`: add an `else` that sets
   `_unsafe_kernel_text_initialized = true; _unsafe_kernel_text = false;` when the loader ADT `/chosen` lacks
   `kernel-ctrr-to-be-enabled` (no CTRR on SM7325), else `ml_unsafe_kernel_text()` asserts in bsd_autoconf (dtrace).
4. **IOMedia publish** — `iokit/Kernel/IOService.cpp` `publishHiddenMedia()`: early-return if the `IOMedia`
   OSMetaClass is absent (no IOStorageFamily kext) instead of asserting; our md0 root is not an IOMedia.
5. **AMFI entitlement** — `iokit/bsddev/IOKitBSDInit.cpp`: add `|| amfi == NULL` to the `proc == NULL` guard in the
   5 entitlement helpers; `amfi` stays NULL without the AMFI kext and the launchd-exec path data-aborted on
   `amfi->OSEntitlements.query...`. With no AMFI nothing is entitled, so returning false is correct (matches the
   codebase's own `amfi == NULL` guards elsewhere).
