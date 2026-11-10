#!/bin/bash
# Build the USB-boot image (out/system-26.apfs.trim) from out/sysroot-26, leaving out
# what does not fit under fastboot's 4 GiB single-download limit: Applications/,
# Library/Wallpaper and System/Library/OnBoardingBundles (~775 MB of the image).
# The directories are moved aside for mkapfs and always moved back (same filesystem,
# no copy). FULL=1 builds the on-flash root instead (out/system-26.apfs.full, nothing left out).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$(cd "$HERE/.." && pwd)"
OUT="$BASE/out"
SR="$OUT/sysroot-26"
ASIDE="$OUT/trimmed-out"
TRIM=(Applications Library/Wallpaper System/Library/OnBoardingBundles)
IMG=system-26.apfs.trim
LIMIT=4294967296                                   # fastboot's single-download limit

# FULL=1: the flash-root image (out/system-26.apfs.full): nothing left out, only bounded by the size of lisa's
# super partition (2228224 blocks of 4 KiB), where it is written raw.
if [ "${FULL:-0}" = 1 ]; then
	TRIM=()
	IMG=system-26.apfs.full
	LIMIT=$((2228224 * 4096 + 1))
fi

# LEAN=1: also leave out the LaunchDaemons listed in lean-daemons.txt (memory; see that file).
LEAN_LIST="$HERE/lean-daemons.txt"
LD=System/Library/LaunchDaemons
# FULL=1 LEAN=1: the flash root without the lean daemons (out/system-26.apfs.full-lean). That is the root that
# reaches the home screen: without locationd the CoreLocation migrator does not hang DataMigrator for 20 min, and
# ClarityBoard/checkerboard do not compete with SpringBoard for the display.
if [ "${LEAN:-0}" = 1 ]; then
	[ "${FULL:-0}" = 1 ] && IMG=system-26.apfs.full-lean
	while read -r f; do
		case "$f" in ''|'#'*) continue;; esac
		TRIM+=("$LD/$f")
	done < "$LEAN_LIST"
fi

restore() {
	for d in "${TRIM[@]+"${TRIM[@]}"}"; do
		[ -e "$ASIDE/$d" ] && { mkdir -p "$(dirname "$SR/$d")"; mv "$ASIDE/$d" "$SR/$d"; }
	done
	# only empty directories go: anything still aside stays where it can be found
	find "$ASIDE" -depth -type d -empty -delete 2>/dev/null || true
}
trap restore EXIT
for d in "${TRIM[@]+"${TRIM[@]}"}"; do
	[ -e "$SR/$d" ] || continue
	mkdir -p "$(dirname "$ASIDE/$d")"
	mv "$SR/$d" "$ASIDE/$d"
done
cd "$BASE"
# WORK: where mkapfs writes the image and its compression spool (~3.5 GB) before the image
# moves to out/ (default out/; a tmpfs saves the disk ~7 GB of peak use).
WORK="${MKAPFS_WORK:-$OUT}"
mkdir -p "$WORK"
python3 -I /home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/tools/apfs/mkapfs.py -z \
	-o "$WORK/$IMG" "System=out/sysroot-26,manifest=extract/system.manifest" | tail -2
rm -f "$WORK/$IMG.spool"
if [ "$WORK" != "$OUT" ]; then rm -f "out/$IMG"; mv "$WORK/$IMG" "out/$IMG"; fi
# Pad to a multiple of 32 MB (sparse; the APFS container ignores the tail). A resident
# ramdisk of 0xEF05C000 bytes (only 16 KB aligned) made the kernel take a UBSAN trap in
# early startup, with the old kernel as well; 0xE0838000 and 0xEF230000 booted.
size=$(stat -c %s "out/$IMG")
truncate -s $(( (size + 0x1FFFFFF) / 0x2000000 * 0x2000000 )) "out/$IMG"
size=$(stat -c %s "out/$IMG")
echo "$IMG: $size bytes"
[ "$size" -lt "$LIMIT" ] || { echo "$IMG is over its limit ($LIMIT bytes)" >&2; exit 1; }
