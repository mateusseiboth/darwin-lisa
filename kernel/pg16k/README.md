# 16 KB pages (iOS-native granule) for the lisa kernel, and what Apple userland needs on top

Every wire script here is idempotent, runs inside the toolchain container as root against
`research/UniXNU/.../xnu-work`, and takes `--revert`. Rebuild with `.work/run-rebuild.sh`.
**`run-rebuild.sh` does not stop on a compile error** (the build step ends in `|| true`), and a
failed build leaves the previous `kernel.macho` in place. After every rebuild, grep
`.../lisa26/logs/kernel.log` for `error:` and grep the new kernel for a string you just added.

## Applied (the boot depends on these)

| wire | what | why |
|---|---|---|
| `pg16k-wire.py` | `__ARM_16K_PG__` for BCM2837; `memdev.c` uses PAGE_SHIFT/PAGE_MASK | 16 KB pages. The shared cache's own 16 KB code signature is accepted as is. |
| `slideauth-wire.py` | `vm_shared_region_slide_page_v3/_v5`: `#else memcpy(...)` | The kernel is built without `ptrauth_calls`, and the slide code only stored *authenticated* rebases inside `#if __has_feature(ptrauth_calls)`. Every auth pointer in the cache's DATA kept its raw chained encoding. |
| `dyldpager-wire.py` | `vm_dyld_pager.c`: compile the arm64e chained-fixup formats without `HAS_APPLE_PAC`, with a `signPointer()` that stores the plain pointer | dyld page-in linking of arm64e binaries failed with `fixup_page(): unknown pointer_format 12` (ARM64E_USERLAND24), and launchd died with SIGBUS. |
| `mmappac-wire.py` | `mmap()`: executable private file mappings go through the PAC-rewriting apple-protect pager | dylibs and bundles dyld loads from disk outside the cache (UserEventAgent/configd plugins) trapped on `BRAA x16, x17` (EXC_BAD_INSTRUCTION, 0xd71f0a11). mach_loader's pager only covers binaries the kernel execs. |
| `watchdogvm-wire.py` | `kern.hv_vmm_present` reads 1 for watchdogd only | No IOWatchdog kext on this port. watchdogd `__os_crash`es (SIGTRAP) unless it sees a VM ("no watchdog KEXT found, exiting...", exit 0, not restarted). launchd marks it `_PanicOnCrash`, so each crash panicked the kernel. |
| `connport-wire.py` | `mach_port_is_connection_for_service()` answers "no policy" (KERN_SUCCESS, filter id 0) when no mach message filter is registered, instead of KERN_NOT_SUPPORTED | Every XPC listener of iOS 26 asks the kernel whether a new client's connection port derives from its service port. Only Apple's Sandbox kext registers the message filter that question needs, so the answer was always 46: libxpc logged "Failed to verify check-in connection port ... (kr=46)" / "Bogus check-in attempt. Ignoring." and dropped every client. All first messages to launchd services came back XPC_INTERRUPTED_REPLY (lsd -> containermanagerd, runningboardd -> cfprefsd): lsd aborted and runningboardd, backboardd and SpringBoard waited for ever. |
| `ramfs-wire.py` (+ `ramfs/ramfs.c`) | HakiOS's in-memory read-write filesystem, registered as `ramfs` (xnu-12377 port: `<vm/vm_kern_xnu.h>`) | The data volume. The APFS driver mounts read only and nothing may be written to flash, so init26 mounts ramfs on /private/var. |
| `maxvnodes-wire.py` | `config/MASTER.arm64.bcm2837`: `CONFIG_VNODES` 1024 -> 263168, so `desiredvnodes` is sized from memory (memsize / 64 KB + 1024, ~88k) | BCM2837 is an "xsmall" config. At speed (cpufreq + several cores) iOS 26 fills 1024 vnodes in minutes ("vnode: table is full"), and jetsam kills daemons for vnodes (`JETSAM_REASON_VNODE`, code 3) about 1000 times in 20 minutes while launchd relaunches them, every core at 100% |

`mach_loader.c` `pac_page_transform` stays at 4096 bytes (the apple-protect pager calls
page_decrypt in 4 KB sub-chunks; PAGE_SIZE there corrupted dyld).

## Superseded: do not apply

`phase3-cachepac-wire.py` (runtime PAC-strip of every executable shared-cache mapping) is
**reverted**. It rewrites whole pages, so it also rewrites data that lives in executable
mappings wherever a 32-bit word happens to encode a PAC instruction: about 900 words per
sub-cache. The visible casualty was the cache's image table. WiFiCloudSyncEngine's address
0x2_9AD43000 has low word 0x9AD43000 = `pacga`, which became `eor ..., ror #13` = 0xCAD43400.
dyld then handed libobjc a mach_header at 0x2_CAD43400, and SpringBoard and UserEventAgent
died with SIGBUS in `map_images`. The cache is now stripped **offline and code-only** by
`userland-26/scripts/pacstrip-cache.py`: image code sections plus the `__TEXT_STUBS` islands,
with the code-slot hashes re-computed. The kernel no longer touches it.

Same class, still open: the mach_loader pager (kernel-exec'd binaries) and the mmap pager
(dyld-loaded files) also rewrite whole pages, `__TEXT,__const` included. Nothing has broken
from it yet; a code-only transform (section ranges per file) would close it.

## Diagnostics (apply when needed, revert before a release)

`diag-uxexc-wire.py` prints every exception that turns into a signal (pid, name, exception,
code, fault address, pc). It also prints the shared-region slide.
`diag-srfault-wire.py` handles a refused access in the shared region: it prints x19/x22/x27/x28
and the `_dyld_objc_notify_mapped_info` entries around x27, which is how WiFiCloudSyncEngine
was found.
`diag-proclife-wire.py` prints `DLISA exec`/`DLISA exit` per process, which is the only view of
the boot once logd takes launchd's log off the console.
`diag-sctrace-wire.py` traces every BSD syscall of pid 1. It is heavy; use it only for dyld/launchd start-up.
`diag-kill-wire.py` prints kill(2) of a single process with SIGTERM/SIGKILL/SIGHUP/SIGINT, every SIGTERM that
reaches psignal or the EVFILT_SIGNAL knotes, and an EVFILT_SIGNAL knote that fires on a non-signal hint.
`diag-kevent-wire.py` prints refused kevent registrations (at most 300) and every EVFILT_SIGNAL(SIGTERM) event
delivered to user space. Both were written chasing "Bogus check-in attempt" (it turned out to be connport, above).
