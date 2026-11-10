#!/bin/bash
# Build the USB-boot image (out/system-26.apfs.trim) from out/sysroot-26, leaving out
# what does not fit under fastboot's 4 GiB single-download limit: Applications/,
# Library/Wallpaper and System/Library/OnBoardingBundles (~775 MB of the image).
# The directories are moved aside for mkapfs and always moved back (same filesystem,
# no copy). The full image (out/system-26.apfs.new) is for the on-flash root later.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$(cd "$HERE/.." && pwd)"
OUT="$BASE/out"
SR="$OUT/sysroot-26"
ASIDE="$OUT/trimmed-out"
TRIM=(Applications Library/Wallpaper System/Library/OnBoardingBundles)

# LEAN=1: also leave out the LaunchDaemons listed in lean-daemons.txt (memory; see that file).
LEAN_LIST="$HERE/lean-daemons.txt"
LD=System/Library/LaunchDaemons
if [ "${LEAN:-0}" = 1 ]; then
	while read -r f; do
		case "$f" in ''|'#'*) continue;; esac
		TRIM+=("$LD/$f")
	done < "$LEAN_LIST"
fi

restore() {
	for d in "${TRIM[@]}"; do
		[ -e "$ASIDE/$d" ] && { mkdir -p "$(dirname "$SR/$d")"; mv "$ASIDE/$d" "$SR/$d"; }
	done
	# only empty directories go: anything still aside stays where it can be found
	find "$ASIDE" -depth -type d -empty -delete 2>/dev/null || true
}
trap restore EXIT
for d in "${TRIM[@]}"; do
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
	-o "$WORK/system-26.apfs.trim" "System=out/sysroot-26,manifest=extract/system.manifest" | tail -2
rm -f "$WORK/system-26.apfs.trim.spool"
if [ "$WORK" != "$OUT" ]; then rm -f out/system-26.apfs.trim; mv "$WORK/system-26.apfs.trim" out/system-26.apfs.trim; fi
# Pad to a multiple of 32 MB (sparse; the APFS container ignores the tail). A resident
# ramdisk of 0xEF05C000 bytes (only 16 KB aligned) made the kernel take a UBSAN trap in
# early startup, with the old kernel as well; 0xE0838000 and 0xEF230000 booted.
size=$(stat -c %s out/system-26.apfs.trim)
truncate -s $(( (size + 0x1FFFFFF) / 0x2000000 * 0x2000000 )) out/system-26.apfs.trim
size=$(stat -c %s out/system-26.apfs.trim)
echo "system-26.apfs.trim: $size bytes"
[ "$size" -lt 4294967296 ] || { echo "over fastboot's 4 GiB download limit" >&2; exit 1; }
