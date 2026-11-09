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
