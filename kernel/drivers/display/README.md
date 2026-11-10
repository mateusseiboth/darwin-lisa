# Display: IOSurface + IOMobileFramebuffer on the boot framebuffer (iOS 26)

HakiOS's two display stand-ins, ported from xnu-8796 (iOS 16.5) to xnu-12377 (iOS 26.5.2), plus the
diagnostics that were needed to find out why the iOS 26 window server never opened them.

The panel (VTDR6130, DSI command mode, DSC) is brought up by the firmware. The kernel only writes into
the framebuffer the firmware left (`vinfo`, 1080x2400, 32 bpp, 4320 bytes per row). There is no GPU
path yet: CoreAnimation is expected to draw with its software renderer, as it did on 16.5.

## Sources

| File | What |
| --- | --- |
| `MiniIOSurface.{cpp,h}` | `IOCoreSurfaceRoot` + `IOSurfaceRootUserClient` for IOSurface.framework |
| `MiniIOMobileFramebuffer.cpp` | `IOMobileFramebuffer` "disp0" + its user client, the `/arm-io/disp0` device-tree node (`display-subsystem`) and the `IOAcceleratorES` stand-in |
| `display-wire.py` | wires both into the xnu-work tree, tag `hakios_display` |
| `dlisa_sample.c`, `diag-iotrace-wire.py` | diagnostics (below) |

Everything is inert unless boot-arg `display=1`.

Wire order on a fresh tree: `../adreno-wire.py`, then `display-wire.py` (it removes the no-op
`MiniIOMobileFramebufferShow` the adreno glue carries; re-running adreno-wire.py afterwards brings the duplicate
back), then `diag-iotrace-wire.py` (diagnostics, optional), and `../cpufreq/cpufreq-wire.py` (optional).
`kernel/patches/` (applied by `.work/rebuild26.sh`) carries the dyld PAC-strip and the quiet video console.

### What changed from 16.5

* **IOSurface replies (iOS 26 layout).** Worked out from IOSurface.framework's own accessors
  (`IOSurfaceClientGet*`, the client keeps the reply at +0x70 of its object):
  * the create/lookup reply header grew from 0x68 to 0x88 bytes: width, height, bytes per row, offset
    and size are 64-bit, and everything from the plane count on is 0x20 further;
  * a plane grew from 0x80 to 0x98 bytes: its first six words are 64-bit, the rest is 0x18 further;
  * the reply buffer is 0xc68 bytes (20 planes);
  * `init` (selector 13) wants 0x28 bytes: two 32-bit alignment masks, three 64-bit maxima, the version
    (1) at 0x22.

  `fillResult26()` and `methodInit()` build these. Selector and trap numbers are unchanged
  (create 0, lookup 4, set value 9, bind 12, init 13, set purgeable 20, bulk 27, lookup-from-port 34,
  create-port 35; traps lock 2, unlock 3, detach 4, release 5).
* **IOMobileFramebuffer swaps.** `SwapEnd` is still selector 5, but the request is 0x588 bytes
  (`IOConnectCallStructMethod(conn, 5, fb+0x18, 0x588)`), and 16.5's layout sits 0x48 further in: swap
  ID 0x98, surfaces 0x9c, source rectangles 0xac, destination rectangles 0x10c, layers set 0x14c.
  The driver takes the shift from the request size (boot-arg `fb_swapshift=` overrides it) and dumps the
  first two requests (`fbswap` lines, `fb_swapdump=N`).
* Started from `bsd_init` (`hakios_display_init()`) on IOResources, because this tree keeps Apple's
  platform expert (HakiOS started them from its GenericARMPE).
* `MiniIOSurfaceDrawn()` is a stub that always answers "drawn" until `hakios.gpu.cpp` is ported.
* `loader_recording[]` is no longer `static` in `kernel/loader-console/loader_console.c`, so the
  framebuffer's screen recording (`rec_secs=N`) is visible to the loader. The 16 KB kernel does not
  return to the loader, so `xl recording` cannot fetch it yet.

## Status (2026-11-10)

* **The display path works end to end with iOS 26's own frameworks.** `fbtest` (`userland/tools/fbtest.c`, an arm64
  program run before launchd with `init_run=/usr/local/bin/fbtest`) finds the display through
  IOMobileFramebuffer.framework, creates a 1125x2436 BGRA IOSurface through IOSurface.framework, draws it with
  CoreGraphics + CoreText and swaps it to the panel. Every layout above was confirmed by those calls (the `fbswap`
  dump: ID 1 at 0x98, surface 1 at 0x9c, 1125x2436 source/destination, layers set 0x1 at 0x14c). Screenshot of the
  panel memory: `research/hackios-xnu-kernel/logs/display-fbtest-ios26.png`.
* Long runs: 45 minutes with display=1 and no panic (`logs/display-run38-shot-45min.png`); fbtest's picture
  repainted for 20 minutes from a background process (`logs/display-run42-shot-10min.png`).
* **The window server (backboardd) does not draw yet**: it never gets far enough to open the display (see below).
  At the end of the night backboardd (`managedEndpointByLaunchIdentifier`) and SpringBoard (`FBSystemShellInitialize`
  -> `acquireAssertion`) are both blocked in synchronous calls to runningboardd, which sits idle in its run loop.

## Seeing the screen without looking at it

* `run-fullroot.sh` with `XL_SHOT_EVERY=N` saves the loader's `oem screenshot` (the panel memory, read
  live from core 0) every N polls as `logs/fullroot-shot-<poll>.png` (`tools/fbraw2png.py`).
* Boot-arg `fb_snap=N`: every N seconds the kernel logs a 45x100 thumbnail of the panel as `fbsnap`
  lines; `tools/fbsnap.py LOG DIR` turns them into PNGs.

## Diagnostics (boot-args, all off by default)

| Boot-arg | Prints |
| --- | --- |
| `psample=A,B,...` `psample_secs=N` | every N s, every thread of the named processes: state, block hint, syscall, user pc/lr and 40 frames (`psample` lines). Symbolize with the dyld cache and the printed slide (`ipsw dsc a2s`). |
| `iotrace=NAME` `iotrace_max=N` | IOKit requests of a process: matching dictionaries, notifications, registry paths, property reads, user-client opens |
| `errtrace=NAME` `errtrace_max=N` | every failing syscall of a process, with its path argument |
| (always) | `DLISA abort` lines: the reason string of every `abort_with_reason`/`terminate_with_reason` |

## Why nothing was drawn on iOS 26 (what the diagnostics found)

1. The services were never opened: no process created an IOSurface and no one opened the framebuffer
   (`iotrace=backboardd` showed backboardd made no IOKit call at all).
2. `psample=backboardd`: backboardd is blocked in BoardServices `bootstrapConfiguration` ->
   RunningBoardServices `managedEndpointByLaunchIdentifier`, a synchronous XPC call to runningboardd.
3. runningboardd's `init` is blocked in UserManagement `personaAttributesForPersonaType:` (synchronous
   NSXPC to usermanagerd).
4. usermanagerd aborts at every start: `Daemon failed to load persona manifest.` It reads
   `/private/var/keybags/persona.kb`, which only its early boot task (`usermanagerd --init`) writes.
5. launchd skips that boot task: it sees `rd=md0` in `kern.bootargs` and runs as a **Restore
   environment** ("Restore environment starting."), where every boot task that is not
   `PerformInRestore` is skipped (keybag, usermanagerd, dirs_cleaner, tzinit, ...).
6. Fixes so far: init26 runs `usermanagerd --init` itself (boot-arg `init_run=`, see
   `userland/init/init-26.c`), creates `/private/var/keybags`, and ramfs accepts `F_BARRIERFSYNC` /
   `F_FULLFSYNC` and `renameatx_np` (`RENAME_EXCL`/`RENAME_SWAP`). usermanagerd now stays up and
   runningboardd finishes its init. **Do not run `keybagd --init` that way**: with no system keybag it
   reboots the phone into recovery ("FATAL KEYBAG ERROR").
7. runningboardd then waits on lsd (CoreServices `applicationProxyForBundleType:`), and lsd aborts at every
   start: `__LSServerMain` -> `userContainerURL` -> "Could not get user container URL! Must abort." The
   container query is class 10, persona `CONTAINER_PERSONA_PRIMARY`, flags 0x900000001
   (`userland/tools/cmquery.c` repeats it from a launchd job and prints containermanagerd's error).
   This is where the chain stands; backboardd is still waiting on runningboardd.

Other fixes found on the way:

* dyld is arm64e only, and the PAC-stripping pager ran only when the *main* program was arm64e, so a plain arm64
  program died on dyld's first RETAB. dyld (`MH_DYLINKER`) now always goes through the pager
  (`kernel/patches/mach_loader-pac-pager.patch`).
* minisandbox answers `sandbox_extension_issue_*` (request 5) with a token (`kernel/security/kern_minimac.c`).
* `cpufreq=1` (HakiOS's EPSS driver, `kernel/drivers/cpufreq/`): the prime core goes from the firmware's clock
  to 2400 MHz (a calibration loop 79 -> 464, ~5.9x). `ddr=0` leaves the memory bus vote alone (not tried on lisa).
* The loader log is 1.5 MB again (256 KB head + 1.25 MB ring); with 256 KB only the first minutes of a boot
  were ever visible.

## Tools and image options

* `userland/tools/fbtest.c` (+ `build-fbtest.sh`): the display check above, installed as `/usr/local/bin/fbtest`.
* `userland/tools/cmquery.c`: repeats lsd's container query and prints containermanagerd's error, or (`cmquery tail
  FILE`) copies a file to the console. Run from a launchd job `com.darwinlisa.cmquery` (root; a job running as
  mobile cannot open `/dev/console`, posix_spawn fails with EACCES). As an unentitled program its query only
  comes back `XPC_INTERRUPTED_REPLY`, so it does not reproduce lsd's case.
* `userland/scripts/mkimage-trim.sh` with `LEAN=1` leaves out the LaunchDaemons in `lean-daemons.txt` (77 services
  for hardware this port does not drive, iCloud/account services and the other UI shells). Memory before the window
  server is the reason: ~1.6 GB for all of user space with the root resident in RAM.
* init26 boot-args: `init_run=PROG[:ARG...][;...]` runs early boot tasks before launchd (`usermanagerd --init`
  is needed: `init_run=/usr/libexec/usermanagerd:--init`); `console_open=1` makes `/dev/console` writable to all.

## Reproduce

```sh
cd research/hackios-xnu-kernel
XL_SECS=1500 XL_POLLS=60 XL_SHOT_EVERY=10 \
XL_EXTRA_ARGS="display=1 init_run=/usr/local/bin/fbtest:1200:bg;/usr/libexec/usermanagerd:--init init_run_secs=60" \
  ./run-fullroot.sh /home/mateusseiboth/Dev/mobile/userland-26/out/system-26.apfs.trim
# logs/fullroot.log: "fbtest: swap N -> 0x0", "MiniIOMobileFramebuffer: swap ...";
# logs/fullroot-shot-*.png: the panel (fbtest's picture from the first screenshot on)
```

`fbtest SECONDS bg` keeps re-presenting its picture once a second from a child process. That matters only while the
host polls the loader: the loader on core 0 prints every fastboot command it serves on the same panel memory, and
after a few dozen polls its text covers the screen. The kernel's own text console stops drawing at the first
frame (`dlisa_vc_quiet`, `kernel/patches/vcputc-tee.patch`); its output still reaches the loader log.

`cpufreq=1` (optionally `cpufreq_max=MHz`) makes the boot several times faster but **the kernel stops a few minutes
into every such boot** (heartbeat lines stop at ~4-5 min, at 2400 and at 1324 MHz alike, with 400 MB still free);
without it the same boot runs for 45 minutes. **Cause (offline analysis, to be confirmed on the device): WFI at idle.**
The kernel has one core and no device interrupt; the virtual timer is its only wake source, and this hypervisor does
not wake a vCPU parked in WFI when the virtual timer fires (`docs/phase3-ondevice.md`, "Timer-at-idle"). At the
firmware's clock the core never runs out of work during a boot; at 1.3-2.4 GHz it does once the daemons settle,
`cpu_idle()` executes WFI, and the core never wakes. The loader then sees no heartbeat for 50 s, calls the kernel hung
and the live log stops (the last kernel lines are ordinary activity, no heartbeat after them). Not the watchdogs (the
loader pets the hypervisor's until `loader_return_after`+60 s), not the timebase (19.2 MHz counter, independent of the
core clock), not voltage (EPSS sets it with the row, as in Linux's qcom-cpufreq-hw; the addresses match lisa's DTB).
Fix prepared: `kernel/patches/cpu-idle-no-wfi.patch` (idle polls instead of WFI; not yet in `apply.sh`), or boot-arg
`wfi=0` on the current kernel. It matters without cpufreq too: a settled system (an idle SpringBoard) hangs the same
way.

## The window server chain after the display (2026-11-10, second night)

What stood between the working display path and backboardd drawing, in the order it was found:

1. **lsd's user container lookup came back `XPC_INTERRUPTED_REPLY`** ("Could not get user container URL! Must
   abort."). The cause was general, not containermanagerd's: every iOS 26 XPC listener asks the kernel whether a new
   client's connection port derives from its service port (`mach_port_is_connection_for_service`), and xnu answers
   KERN_NOT_SUPPORTED unless a mach message filter is registered, which only Apple's Sandbox kext does. libxpc logged
   "Failed to verify check-in connection port ... (kr=46)" / "Bogus check-in attempt. Ignoring." and dropped every
   client of every launchd service. Fix: `kernel/pg16k/connport-wire.py` (no filter = no policy, KERN_SUCCESS).
2. **backboardd aborted at every start** in libxpc: `assertion failure: "matched == (match_result.error_code ==
   AICMR_MATCH)" -> 0`. XPC session peer requirements are checked with AMFI request 0x66, which miniamfi did not
   answer. Fix: `kernel/security/amfi-lcmatch-wire.py` (every process matches).
3. HakiOS's `personasAreSupported -> NO` for containermanagerd is carried in `userland/inject/libdlisa.dylib`; it was
   not the cause of (1) and is kept because 16.5 needed it.

Tools written on the way (all kept):

* `kernel/execaids/execaids-wire.py`: `kern.exec_inject` (DYLD_INSERT_LIBRARIES by program name; init26 sets it
  from `/etc/dlisa-inject.conf`) and `applog=1 applog_name=A,B` (OS_ACTIVITY_DT_MODE by name). `applog_console=1`
  (HakiOS's /dev/null -> /dev/console for spawned programs) makes every launchd spawn fail with EACCES on iOS 26,
  so it is opt-in.
* `userland/inject/` (libdlisa.dylib): boot-arg `dlisa_oslog=all|NAME,NAME` copies the os_log of the programs it is
  in to the console. This is what found (1): `oslog[cfprefsd] ... Bogus check-in attempt`.
* `kernel/drivers/keystore/` (MiniAppleKeyStore from HakiOS, boot-arg `keystore=1`): containermanagerd and others
  log "failed to open connection to AppleKeyStore" without it. Not the cause of (1); harmless with it.
* `kernel/pg16k/diag-kill-wire.py`, `diag-kevent-wire.py`: signal / kevent diagnostics (see that README).
