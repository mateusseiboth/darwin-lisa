#!/bin/sh
# The T1 test root: the phone's current root (BASE, default 9a91ed47: int3 + bluetoothd) with AdrenoMetal.bundle beside
# today's HakiosMetal.bundle. Nothing else changes, so without boot-arg metal_plugin=AdrenoMetal the root boots today's driver.
#
# The recipe of the earlier roots (docs/peripherals.md, root 9a91ed47): a hard-link copy of out/sysroot-26 (the shared sysroot
# is never written: every file put in is unlinked first), BASE's own files taken out of BASE with apfs.py cat, then
# mkapfs.py -z with extract/system.manifest, padded to 32 MiB, read back. The copy is out/.root-t1 (hard links need the
# sysroot's file system; deleted after); the image and mkapfs's spool are made in MKAPFS_WORK (a tmpfs saves the disk ~8 GB
# of peak use) and the image moved to out/ at the end.
# EXTRA="PATH=FILE ..." puts more files in after BASE's (int4: the libdlisa and libdlisa-motion of later roots); each is
# read back against its FILE.
#   build-root.sh   -> ~/Dev/mobile/userland-26/out/system-26.apfs.full-<sha8>
set -eu
M=$HOME/Dev/mobile
U=$M/userland-26
UNIXNU=${UNIXNU:-$U/gpu-build/unixnu}
APFS=$M/research/hackios-xnu-kernel/tools/apfs
BASE=${BASE:-$U/out/system-26.apfs.full-9a91ed47}
WORK=${MKAPFS_WORK:?MKAPFS_WORK= a directory with ~9 GB free (tmpfs is best)}
R=$U/out/.root-t1
HERE=$(cd "$(dirname "$0")" && pwd)
PLUGIN=$UNIXNU/out/plugin
[ -f "$PLUGIN/AdrenoMetal.bundle/AdrenoMetal" ] || { echo "build-root: no plug-in (build-plugin.sh)"; exit 1; }
# BASE's files that differ from out/sysroot-26 (docs/peripherals.md: int3's six, bluetoothd and the conf)
OWN="usr/local/lib/libdlisa.dylib usr/local/lib/libdlisa-motion.dylib private/etc/dlisa-ld-groups.conf
private/etc/dlisa-inject.conf sbin/launchd System/Library/Extensions/HakiosMetal.bundle/HakiosMetal
System/Library/SystemConfiguration/InterfaceNamer.bundle/Info.plist usr/sbin/bluetoothd"
rm -rf "$R"
mkdir -p "$WORK"
cp -al "$U/out/sysroot-26" "$R"
put() {        # put SOURCE PATH: a file of its own at PATH (never the sysroot's inode)
	rm -f "$R/$2"
	mkdir -p "$(dirname "$R/$2")"
	cp "$1" "$R/$2"
}
for p in $OWN; do
	python3 -I "$APFS/apfs.py" cat "$BASE" "/$p" > "$WORK/own"
	mode=$(stat -c %a "$U/out/sysroot-26/$p" 2>/dev/null || echo 755)
	put "$WORK/own" "$p"
	chmod "$mode" "$R/$p"
done
rm -f "$WORK/own"
for e in ${EXTRA:-}; do
	p=${e%%=*}
	mode=$(stat -c %a "$R/$p" 2>/dev/null || echo 755)
	put "${e#*=}" "$p"
	chmod "$mode" "$R/$p"
done
B=System/Library/Extensions/AdrenoMetal.bundle
mkdir -p "$R/$B"
for f in AdrenoMetal libvulkan_freedreno.dylib Info.plist; do put "$PLUGIN/AdrenoMetal.bundle/$f" "$B/$f"; done
chmod 755 "$R/$B/AdrenoMetal" "$R/$B/libvulkan_freedreno.dylib"
chmod 644 "$R/$B/Info.plist"
put "$PLUGIN/NativeAdrenoMetal.clients" System/Library/Extensions/NativeAdrenoMetal.clients
chmod 644 "$R/System/Library/Extensions/NativeAdrenoMetal.clients"
# metal2vulkan's licence (GPL-2.0-or-later OR LGPL-3.0-or-later): where the source of what is in the bundle is
put "$HERE/SOURCES" "$B/SOURCES"
chmod 644 "$R/$B/SOURCES"
cd "$U"
python3 -I "$APFS/mkapfs.py" -z -o "$WORK/system.apfs" "System=$R,manifest=extract/system.manifest" | tail -2
rm -f "$WORK/system.apfs.spool"
rm -rf "$R"
size=$(stat -c %s "$WORK/system.apfs")
truncate -s $(( (size + 0x1FFFFFF) / 0x2000000 * 0x2000000 )) "$WORK/system.apfs"
sum=$(sha256sum "$WORK/system.apfs" | cut -c1-64)
IMG=$U/out/system-26.apfs.full-$(echo "$sum" | cut -c1-8)
mv "$WORK/system.apfs" "$IMG"
echo "$sum  $(basename "$IMG")" > "$IMG.sha256"
echo "$(basename "$IMG"): $(stat -c %s "$IMG") bytes, sha256 $sum"
# read back: every file put in, against its source
for p in $OWN; do
	case " ${EXTRA:-} " in *" $p="*) continue ;; esac      # (put in from EXTRA: checked below)
	a=$(python3 -I "$APFS/apfs.py" cat "$IMG" "/$p" | sha256sum | cut -c1-8)
	b=$(python3 -I "$APFS/apfs.py" cat "$BASE" "/$p" | sha256sum | cut -c1-8)
	[ "$a" = "$b" ] || { echo "build-root: /$p reads back $a, BASE has $b"; exit 1; }
	echo "  /$p $a"
done
for e in ${EXTRA:-}; do
	a=$(python3 -I "$APFS/apfs.py" cat "$IMG" "/${e%%=*}" | sha256sum | cut -c1-8)
	b=$(sha256sum < "${e#*=}" | cut -c1-8)
	[ "$a" = "$b" ] || { echo "build-root: /${e%%=*} reads back $a, put in $b"; exit 1; }
	echo "  /${e%%=*} $a (EXTRA)"
done
for f in AdrenoMetal libvulkan_freedreno.dylib Info.plist SOURCES; do
	a=$(python3 -I "$APFS/apfs.py" cat "$IMG" "/$B/$f" | sha256sum | cut -c1-8)
	src=$PLUGIN/AdrenoMetal.bundle/$f
	[ "$f" = SOURCES ] && src=$HERE/SOURCES
	b=$(sha256sum < "$src" | cut -c1-8)
	[ "$a" = "$b" ] || { echo "build-root: /$B/$f reads back $a, built $b"; exit 1; }
	echo "  /$B/$f $a"
done
python3 -I "$APFS/apfs.py" census "$IMG" | tail -3
