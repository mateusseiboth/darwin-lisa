# iOS 26 userland build (Caminho B)

Reproducible flow to build the in-RAM APFS root our kernel's built-in APFS driver mounts. Outputs
(images, decrypted dmgs) live outside git under ~/Dev/mobile/userland-26/out/ ; these are the scripts.

- `scripts/build-root-26.sh` — builds the minimal first-boot root: a tiny static no-PAC /sbin/launchd
  (init/init-26.c + HakiOS libmini raw syscalls, linked static LC_UNIXTHREAD with cctools ld) packaged into
  a 4 MB APFS container by HakiOS's tools/apfs/mkapfs.py. Proves mount + userland-exec with no dyld/pac-pager.
- `scripts/build-system-26.sh` — the full iOS-26 root recipe (validated piecemeal): decrypt the AEA-encrypted
  FileSystem + SystemOS cryptex (keys auto-fetched from Apple's wkms server via `ipsw fw aea`), 7z-extract the
  selected files (select-system-26.py), lay the cryptex content + dyld shared cache at
  /private/preboot/Cryptexes/{OS,App} (iOS 26 ships /System/Cryptexes/* as symlinks into an empty
  /private/preboot), keep Apple's arm64e binaries unmodified (pac-pager + miniamfi handle them at runtime),
  mkapfs -> system-26.apfs. Needs ~35 GB transient disk.
- `scripts/select-system-26.py` — the iOS-26-adapted file selector (what of the 8 GB root goes in RAM).
- `init/` — the init variants (init-26.c freestanding, stage0.c, mini.c libmini) + build-init.sh.
- `log/ios26-layout.txt` — the iOS-26 rootfs/cryptex/dyld layout map.

## Build result (23F84, verified)

`out/system-26.apfs` — 3592 MB (3,766,702,080 bytes), one mkapfs `System` volume: 65688 files,
24183 dirs, 821 symlinks. Ships OUR static arm64 init as `/sbin/launchd` (round-trips byte-for-byte)
and keeps Apple's arm64e launchd as `/sbin/launchd.darwin` (inflates to a valid arm64e Mach-O). The
dyld shared cache (35 clean parts) sits at `/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/`,
reachable via the `/System/Cryptexes/OS` symlink; 621 `LaunchDaemons` are present, so Apple's launchd
can be swapped in for the next step. >63 MB, so it loads over USB via the loader's `ramdisk` path
(not the mkpayload payload), mounted as `/dev/md0`. Verified: apfs-tool reads every volume;
verify.py content sample = 400/400 byte-identical, block checksums clean.

### Two iOS-26 findings folded into the scripts

1. **Selector dropped the structural cryptex symlinks.** `/System/Cryptexes/{OS,App,ExclaveOS,Supplemental}`
   live directly under `System/` (outside the selector's `System/Library/` whitelist) and were being cut,
   but the dyld-cache path and dozens of rootfs framework symlinks resolve THROUGH them. Fixed in
   `select-system-26.py` (keeps `System/Cryptexes/*`); they go into the image as manifest-only symlinks.
2. **The SystemOS/AppOS cryptex dmgs have no `root/` wrapper** on 26.5.2 — top level is `System` + `usr`
   directly (iOS 16 wrapped them in `root/`). `build-system-26.sh` step 3 updated accordingly, and it now
   prefers the pre-extracted split dyld cache.

### Open item — cryptex LZFSE decmpfs stragglers (post-boot only)

The cryptex Mach-O **executables** (WebKit/JSC framework stubs; the Safari web daemons browserkitd,
webbookmarksd, safarifetcherd, …) are decmpfs-compressed with an LZVN/LZFSE-family type that **neither
7-Zip 26.04 nor `ipsw extract --files` inflates on Linux** — 7z writes a 0-byte main fork, and `ipsw
--files` mounts the dmg and hits the same decmpfs `input/output error` apfs-fuse does. So the cryptex
RESOURCES (plists, nibs, .car, .loctable) are correct but ~2432 cryptex binaries (1593 OS + 839 App) are
empty in the image. This is the Safari/web stack, loaded far past first boot; the framework CODE is in
the dyld shared cache, and the launchd/dyld boot path does not touch these binaries. (The main rootfs is
unaffected: its decmpfs is zlib, which 7z inflates perfectly — verify.py confirms 0 stragglers there.)
To finish them later: pull each file's `:com.apple.decmpfs` + `:com.apple.ResourceFork` alternate streams
with 7z and inflate with an LZVN/LZFSE decmpfs decoder.

## Display work additions (2026-11-10)

- `init/init-26.c`: boot-arg `init_run=PROG[:ARG...][;...]` runs early boot tasks before Apple's launchd (launchd
  runs as a *Restore environment* because of `rd=md0` and skips them); `usermanagerd --init` is the one needed
  (`init_run=/usr/libexec/usermanagerd:--init`). It also creates `/private/var/keybags` (where usermanagerd keeps
  its user session and persona manifest), and `console_open=1` makes `/dev/console` writable to everyone.
  Never run `keybagd --init` that way: without a system keybag it reboots the phone into recovery.
- `scripts/mkimage-trim.sh LEAN=1` leaves out the LaunchDaemons listed in `scripts/lean-daemons.txt` (memory).
- `tools/fbtest.c` + `tools/build-fbtest.sh`: draws through IOSurface/CoreGraphics/IOMobileFramebuffer of iOS 26
  (`/usr/local/bin/fbtest`). `tools/cmquery.c`: containermanagerd query/console tail diagnostics.
- See `kernel/drivers/display/README.md` for the whole picture.

## Data volume on the flash (2026-11-10, not on the phone yet)

`init/init-26.c`: with boot-arg `var_ufs=1`, `/private/var` is the journaled HFSX volume on `/dev/ufs_data`, kept
between boots. That device is super's tail, given out by the loader and the kernel with `ufs_write=1 ufs_data=super`.
If the mount fails, init falls back to ramfs. On the kept volume, init lays down only the missing skeleton entries,
empties `tmp`/`run`/`vm`, and syncs every `var_sync=` s (default 30). `var_grow=1` grows the volume to the whole
device once. The image is made by `tools/mkvarvol.py` and written by `tools/var-write.sh`; the full run book is
`docs/data-volume.md`. A new init only reaches the phone inside a rebuilt root image (`FULL=1 mkimage-trim.sh`).

## Window-server chain additions (2026-11-10)

- `inject/` — **libdlisa.dylib**, our code inside stock daemons (the iOS 26 counterpart of HakiOS's libhakios).
  `inject/dlisa-inject.conf` (installed as `/etc/dlisa-inject.conf`) names the programs; init26 hands it to the
  kernel's `kern.exec_inject` (`kernel/execaids/`), which adds `DYLD_INSERT_LIBRARIES` at exec (launchd does not pass
  DYLD_ variables from a job). `inject/build-inject.sh` builds it (arm64, chained fixups, ad hoc) and
  `inject/arm64e-mark.py` relabels it arm64e (dyld only inserts a library of the process's own architecture; lld
  writes cpusubtype ALL) and re-hashes the signature. What it does per program:
  - containermanagerd: `+[MCMUserIdentityCache personasAreSupported]` -> NO (HakiOS's 16.5 fix), plus tracing of
    the command dispatcher.
  - backboardd: the touch screen. A virtual IOHID service ("darwin-lisa touch screen", digitizer, built in, display
    integrated) made inside backboardd turns the samples of the kernel's /dev/hakios-touch into digitizer events
    (HakiOS 16.5's way; private IOKit HID functions found with dlsym). See `kernel/drivers/touch/README.md`.
  - lsd: repeats lsd's own user container lookup (class 10, primary persona, flags 0x900000001) from inside lsd and
    prints containermanagerd's answer; CoreServices branches straight into libsystem_containermanager, so the
    lookup cannot be interposed.
  - SpringBoard and Preferences: `-[CALayerHost setRendersAsynchronously:]` always gets NO. **A workaround for the
    software renderer:** an asynchronous layer host makes backboardd's CoreAnimation render it into a surface of its
    own (`CA::OGL::AsynchronousNode`), which the CPU context does not track, and backboardd aborts in
    `CA::OGL::Context::push_surface` (`found != slist->end ()`): freeze, then the lock screen again. Boot-arg
    `dlisa_async=1` leaves the hook out. **Re-enable asynchronous rendering (drop the hook, or make `dlisa_async=1` the
    default) once CoreAnimation renders on the GPU** (Metal on the Adreno), where the asynchronous path is the one
    real iPhones use.
  - everyone it is in: stderr points at the console, and boot-arg `dlisa_oslog=all` / `dlisa_oslog=A,B` copies
    every os_log message of those programs to the console (`os_log_set_hook` + `os_log_copy_message_string`).
    `OS_ACTIVITY_DT_MODE` does nothing on a customer build (libtrace honours it only on an internal one).
- `scripts/plist-tweaks.py` — sysroot edits, applied to `out/sysroot-26` in place by `mkimage-trim.sh` before every
  image (tests: `scripts/test_plist_tweaks.py`). It writes
  `System/Library/EventTimingProfiles/D22.Default.plist` (UIKit's default event timing profile for the board the
  loader names; without it UIKit shows a "Misconfigured Device" alert) and `/etc/dlisa-ld-groups.conf` (the groups of
  init26's `ld_disable=`, below). LaunchDaemon edits (originals kept in
  `out/plist-orig/`): `EnablePressuredExit` off for containermanagerd (+ the system one). This was a wrong lead
  (the SIGTERM it guarded against was never sent; the real cause was the kernel's connection-port check, see
  `kernel/pg16k/connport-wire.py`), kept because it is harmless; revert it if it ever matters.
- The lsd / containermanagerd chain was a kernel problem in the end (`kernel/drivers/display/README.md`, "The window
  server chain after the display"); with it fixed lsd gets its container and backboardd draws the boot progress bar.

## Leaving LaunchDaemons out per boot: `ld_disable=` (2026-11-10)

init26 writes launchd's disabled-services file for the system domain,
`/private/var/db/com.apple.xpc.launchd/disabled.plist` (label -> `true`), at every boot (`init/ld-disable.c`, host
tests `init/test/run-tests.sh`). What goes in:

- launchd's own defaults (dhcp6d and bootpd off, ftp-proxy-embedded on);
- the `default` group of `/etc/dlisa-ld-groups.conf`: the services this port always leaves off. Today
  `com.apple.CommCenter`: with no baseband it runs but never answers, and SpringBoard's main thread waits on it for good
  at startup (`-[SBTelephonyManager isInEmergencyCallbackMode]`, a synchronous XPC). That is why the full image never
  drew a frame. `ld_default=0` keeps them on;
- boot-arg `ld_disable=TOKEN,TOKEN,...`: a group (`lean` = all of `scripts/lean-daemons.txt`, `lean1`..`lean8` in
  chunks of 10, `ui` = ClarityBoard / checkerboard / devicerecoveryassistant / AutomationModeUI), a full label
  (`com.apple.locationd`) or a short one (`locationd`).

A list never outlives the boot that asked for it, so a host run can bisect the full image with boot-args alone, with no
new root. The groups file is made by `plist-tweaks.py` (`ALWAYS_DISABLED` is the `default` group).

