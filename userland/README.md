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
