#!/bin/bash
# Build the iOS 26.5.2 (23F84) in-RAM root filesystem image for the lisa port.
#
# This adapts HakiOS's iOS-16 flow (sysimage/unpack.sh + userland/build.sh) to
# iOS 26. It produces two APFS containers, both made by tools/apfs/mkapfs.py so
# the port's own APFS driver can read them:
#
#   root-26.apfs    the SMALL boot image (our static /sbin/launchd + a release
#                   file). Already built and verified by scripts/build-root-26.sh;
#                   travels in the loader payload (<63 MB) as /dev/md0.
#   system-26.apfs  the FULL root: the chosen part of the iOS 26 userland at its
#                   real paths, the two cryptexes at /private/preboot/Cryptexes,
#                   the dyld shared cache, and our init as /sbin/launchd (Apple's
#                   real one kept as /sbin/launchd.darwin). ~2.5-3 GB, uploaded
#                   over USB with the loader's `ramdisk` path, mounted as /dev/md0.
#
# What is genuinely different from iOS 16, and why each step exists, is written
# next to the step. Run it with the IPSW path:
#
#   build-system-26.sh /path/to/iPad17,1,..._26.5.2_23F84_Restore.ipsw
#
# Needs: the blacktop `ipsw` tool (AEA decrypt + Apple key fetch; NETWORK), 7-Zip
# 23+ (reads APFS), python3, and the docker toolchain for the init. About 35 GB
# of free disk while it runs. Downloaded/decrypted IPSW content is untrusted: it
# stays under $EXTRACT, scripts stay under $SCRIPTS, paths are passed as args.
set -euo pipefail

IPSW="${1:?usage: build-system-26.sh <IPSW>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$(cd "$HERE/.." && pwd)"                     # userland-26/
HK=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel
MKAPFS="$HK/tools/apfs/mkapfs.py"
APFSTOOL="$HK/apfs-work/apfs-tool"                 # the kernel's own apfs_lib reader
IPSW_BIN="${IPSW_BIN:-/home/mateusseiboth/Dev/mobile/tools/ipsw-dl/ipsw}"
EXTRACT="$BASE/extract"
OUT="$BASE/out"
SYSROOT="$BASE/out/sysroot-26"
export TMPDIR="$EXTRACT/tmp"                       # NOT /tmp: /tmp has a quota too small for the 8 GB rootfs
mkdir -p "$EXTRACT" "$OUT" "$TMPDIR"

# iOS 26 IPSW member ids for this build (from `ipsw info`): the filesystem is
# AEA-encrypted now, and so is the OS cryptex; the App cryptex is a plain dmg.
TAG="23F84__iPad17,1_2_3_4"
FS_AEA="$EXTRACT/$TAG/094-56106-108.dmg.aea"       # FileSystem   -> /
SYS_AEA="$EXTRACT/$TAG/094-54821-109.dmg.aea"      # SystemOS     -> /System/Cryptexes/OS  (dyld cache lives here)
APP_DMG="$EXTRACT/$TAG/094-54871-112.dmg"          # AppOS        -> /System/Cryptexes/App  (Safari)
FS_DMG="${FS_AEA%.aea}"
SYS_DMG="${SYS_AEA%.aea}"

# ---------------------------------------------------------------- 0. the init
# A static arm64 Mach-O (LC_UNIXTHREAD, no LC_LOAD_DYLINKER): the kernel execs it
# directly, no dyld. Apple's /sbin/launchd is arm64e and dyld-linked, so it needs
# the whole shared cache resolving before it can print a thing; ours proves the
# image mounts and userland runs first. Built by the sibling script.
"$HERE/build-root-26.sh" init-only

# ------------------------------------------------------------- 1. decrypt + list
# iOS 16 could `7z l` the rootfs straight. iOS 26's rootfs is an AEA1 archive:
# `ipsw` pulls it out of the zip AND fetches the per-file decryption key from
# Apple's wkms key server (needs network), then decrypts to a plain APFS dmg.
if [ ! -f "$FS_DMG" ]; then
	"$IPSW_BIN" extract --dmg fs  -o "$EXTRACT" "$IPSW"
	"$IPSW_BIN" fw aea "$FS_AEA" -o "$EXTRACT/$TAG"     # auto-fetches the archive key online
	rm -f "$FS_AEA"
fi
if [ ! -f "$SYS_DMG" ]; then
	"$IPSW_BIN" extract --dmg sys --fcs-key -o "$EXTRACT" "$IPSW"   # --fcs-key writes the .pem beside it
	"$IPSW_BIN" fw aea "$SYS_AEA" -p "$SYS_AEA.pem" -o "$EXTRACT/$TAG"
	rm -f "$SYS_AEA"
fi
[ -f "$APP_DMG" ] || "$IPSW_BIN" extract --dmg app -o "$EXTRACT" "$IPSW"

# the technical listing the selector reads (7-Zip 23+ reads APFS: 231k files)
7z l -slt "$FS_DMG" > "$EXTRACT/rootfs.slt"
python3 -I "$HERE/select-system-26.py" "$EXTRACT/rootfs.slt" \
	"$EXTRACT/system.files" "$EXTRACT/system.manifest"

# ------------------------------------------------------------- 2. extract rootfs
# Only the chosen files (system.files), to $SYSROOT. 7z restores neither owners
# nor every symlink on Linux; system.manifest carries those into mkapfs.
# NOTE: iOS-26 decmpfs uses LZFSE/LZBITMAP; 7-Zip 26 returns those as an empty
# file plus :com.apple.decmpfs/:com.apple.ResourceFork attrs. HakiOS's unzbm
# finishes LZBITMAP; verify no compressed stragglers remain before mkapfs (a
# silently-empty dylib is how a boot dies deep in a log).
rm -rf "$SYSROOT"; mkdir -p "$SYSROOT"
7z x -y -o"$SYSROOT" "$FS_DMG" @"$EXTRACT/system.files" > "$EXTRACT/extract-rootfs.log"
# (apfs-fuse is NOT usable here: this build's apfs-fuse throws EIO on decmpfs files.)

# ---------------------------------------------------- 3. the cryptexes + dyld cache
# THE iOS-26 STRUCTURAL CHANGE. On iOS 16 /System/Cryptexes/OS was a real dir and
# HakiOS put the cryptex there. On iOS 26 the rootfs ships
#     /System/Cryptexes/OS  ->  ../../private/preboot/Cryptexes/OS   (a symlink)
# and /private/preboot is empty. So the cryptex content and the dyld shared cache
# go at the SYMLINK TARGET, /private/preboot/Cryptexes/OS, where dyld follows the
# link to find them. Same for App.
OS_DST="$SYSROOT/private/preboot/Cryptexes/OS"
APP_DST="$SYSROOT/private/preboot/Cryptexes/App"
mkdir -p "$OS_DST" "$APP_DST"
# the OS cryptex is laid out under a top-level "root/" inside its dmg; graft its
# root/ at the target. Everything except the cache and the DriverKit cache.
7z x -y -o"$EXTRACT/cryptex-os" "$SYS_DMG" 'root' '-xr!com.apple.dyld' '-x!root/System/DriverKit' \
	> "$EXTRACT/extract-cryptex-os.log" 2>&1 || true
cp -a "$EXTRACT/cryptex-os/root/." "$OS_DST/"
# the dyld shared cache at its runtime path; its .symbols stub is never mapped.
# (the split arm64e cache is also pre-extracted at
#  ipsw/26.5.2-extracted/23F84__iPad17,1_2_3_4/ if you prefer that copy.)
D="$OS_DST/System/Library/Caches/com.apple.dyld"
mkdir -p "$D"
7z x -y -o"$EXTRACT/cryptex-cache" "$SYS_DMG" 'root/System/Library/Caches/com.apple.dyld' \
	'-xr!*.symbols*' > "$EXTRACT/extract-dyld.log" 2>&1 || true
cp -a "$EXTRACT/cryptex-cache/root/System/Library/Caches/com.apple.dyld/." "$D/"
# the App cryptex (plain dmg), the same way
7z x -y -o"$EXTRACT/cryptex-app" "$APP_DMG" 'root' > "$EXTRACT/extract-cryptex-app.log" 2>&1 || true
cp -a "$EXTRACT/cryptex-app/root/." "$APP_DST/"
echo "dyld cache placed: $(ls "$D" | grep -c dyld_shared_cache) parts"

# --------------------------------------------------------------- 4. the init swap
# The applications Darwin would install on first boot are system apps here.
if [ -d "$SYSROOT/private/var/staged_system_apps" ]; then
	mv "$SYSROOT/private/var/staged_system_apps"/* "$SYSROOT/Applications/" 2>/dev/null || true
fi
# Our init first; keep Apple's real launchd for when we switch to the full path.
mv "$SYSROOT/sbin/launchd" "$SYSROOT/sbin/launchd.darwin"
install -m 0755 "$OUT/launchd" "$SYSROOT/sbin/launchd"
printf 'HakiOS iOS 26.5.2 (23F84) full system image, lisa / SM7325\n' > "$SYSROOT/etc/hakios-release"

# --------------------------------------------------------------- 5. the image
# zlib where it saves room, exactly as Apple keeps the root volume; the manifest
# restores the rootfs files' owners/modes/symlinks that Linux extraction dropped.
python3 -I "$MKAPFS" -z -o "$OUT/system-26.apfs" \
	"System=$SYSROOT,manifest=$EXTRACT/system.manifest" | tail -1

# --------------------------------------------------------------- 6. verify
# the kernel's own apfs_lib must read every volume back (this is the reader our
# driver is co-designed with). apfsck (linux-apfs) is the second opinion, if built.
"$APFSTOOL" info "$OUT/system-26.apfs"
"$APFSTOOL" stat "$OUT/system-26.apfs" /sbin/launchd
"$APFSTOOL" stat "$OUT/system-26.apfs" \
	/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e
echo "system-26.apfs: $(( $(stat -c %s "$OUT/system-26.apfs") / 1048576 )) MB"
