#!/bin/sh
# The speakers' test root: the phone's current root (BASE, default int14 215d11f6) with
#   /Library/Audio/Plug-Ins/HAL/DLisaAudio.driver   (build-audio.sh: the speakers' AudioServerPlugIn for audiomxd)
#   /usr/local/lib/libdlisa.dylib                   (LIBDLISA: built from the HEAD this script is run from)
#   /private/etc/dlisa-inject.conf                  (HEAD's: audiomxd added)
# Nothing else changes: without boot-args (dlisa_audio=0, no sound=4) the root behaves as BASE but for libdlisa's
# lines in audiomxd and the plug-in's in the log.
#
# The recipe of the earlier roots (kernel/drivers/gpu/unixnu/build-root.sh): a hard-link copy of out/sysroot-26 (never
# written: every file put in is unlinked first), BASE's own files taken out of BASE with apfs.py cat (the list below:
# int3's six, bluetoothd, the conf, AdrenoMetal's bundle), files of the sysroot newer than BASE left out (they are not
# in BASE: another worker's), then mkapfs.py -z with extract/system.manifest, padded to 32 MiB, read back.
#   MKAPFS_WORK=/dev/shm/x LIBDLISA=.../libdlisa.dylib build-root-audio.sh -> userland-26/out/system-26.apfs.full-<sha8>
set -eu
M=$HOME/Dev/mobile
U=$M/userland-26
APFS=$M/research/hackios-xnu-kernel/tools/apfs
BASE=${BASE:-$U/out/system-26.apfs.full-215d11f6}
WORK=${MKAPFS_WORK:?MKAPFS_WORK= a directory with ~9 GB free (tmpfs is best)}
LIBDLISA=${LIBDLISA:?LIBDLISA= libdlisa.dylib built from HEAD}
HERE=$(cd "$(dirname "$0")" && pwd)
DRIVER=$HERE/out/DLisaAudio.driver
CONF=$HERE/../inject/dlisa-inject.conf
R=$U/out/.root-audio
[ -f "$DRIVER/DLisaAudio" ] || { echo "build-root-audio: no plug-in (build-audio.sh)"; exit 1; }
[ -f "$BASE" ] || { echo "build-root-audio: no BASE $BASE"; exit 1; }
OWN="usr/local/lib/libdlisa-motion.dylib private/etc/dlisa-ld-groups.conf sbin/launchd
System/Library/Extensions/HakiosMetal.bundle/HakiosMetal System/Library/SystemConfiguration/InterfaceNamer.bundle/Info.plist
usr/sbin/bluetoothd System/Library/Extensions/AdrenoMetal.bundle/AdrenoMetal
System/Library/Extensions/AdrenoMetal.bundle/libvulkan_freedreno.dylib System/Library/Extensions/AdrenoMetal.bundle/Info.plist
System/Library/Extensions/AdrenoMetal.bundle/SOURCES System/Library/Extensions/NativeAdrenoMetal.clients"
D=Library/Audio/Plug-Ins/HAL/DLisaAudio.driver
EXTRA="usr/local/lib/libdlisa.dylib=$LIBDLISA private/etc/dlisa-inject.conf=$CONF $D/DLisaAudio=$DRIVER/DLisaAudio
$D/Info.plist=$DRIVER/Info.plist"
for library in $(awk '{ print $1 }' "$CONF"); do
	case " $OWN $EXTRA " in *" ${library#/}"*) ;; *) [ -f "$U/out/sysroot-26$library" ] ||
	    { echo "build-root-audio: the conf names $library, which the root would lack"; exit 1; } ;; esac
done
rm -rf "$R"
mkdir -p "$WORK"
cp -al "$U/out/sysroot-26" "$R"
put() {        # put SOURCE PATH: a file of its own at PATH (never the sysroot's inode)
	rm -f "$R/$2"
	mkdir -p "$(dirname "$R/$2")"
	cp "$1" "$R/$2"
}
# the sysroot's files changed after BASE was made (another worker's): BASE's version if BASE has the file, else left out
find "$R" -type f -newer "$BASE" -print > "$WORK/newer"
while read -r f; do
	p=${f#$R/}
	if python3 -I "$APFS/apfs.py" cat "$BASE" "/$p" > "$WORK/own" 2>/dev/null; then
		mode=$(stat -c %a "$f")
		put "$WORK/own" "$p"
		chmod "$mode" "$R/$p"
		echo "  /$p: newer in the sysroot than BASE, BASE's taken"
	else
		rm -f "$f"
		echo "  /$p: newer in the sysroot than BASE and not in it, left out"
	fi
done < "$WORK/newer"
rm -f "$WORK/newer" "$WORK/own"
for p in $OWN; do
	python3 -I "$APFS/apfs.py" cat "$BASE" "/$p" > "$WORK/own"
	mode=$(stat -c %a "$U/out/sysroot-26/$p" 2>/dev/null || echo 755)
	case $p in *.plist|*.conf|*.clients|*/SOURCES) mode=644 ;; esac
	put "$WORK/own" "$p"
	chmod "$mode" "$R/$p"
done
rm -f "$WORK/own"
for e in $EXTRA; do
	p=${e%%=*}
	mode=755
	case $p in *.plist|*.conf) mode=644 ;; esac
	put "${e#*=}" "$p"
	chmod "$mode" "$R/$p"
done
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
	a=$(python3 -I "$APFS/apfs.py" cat "$IMG" "/$p" | sha256sum | cut -c1-8)
	b=$(python3 -I "$APFS/apfs.py" cat "$BASE" "/$p" | sha256sum | cut -c1-8)
	[ "$a" = "$b" ] || { echo "build-root-audio: /$p reads back $a, BASE has $b"; exit 1; }
	echo "  /$p $a (BASE)"
done
for e in $EXTRA; do
	a=$(python3 -I "$APFS/apfs.py" cat "$IMG" "/${e%%=*}" | sha256sum | cut -c1-8)
	b=$(sha256sum < "${e#*=}" | cut -c1-8)
	[ "$a" = "$b" ] || { echo "build-root-audio: /${e%%=*} reads back $a, put in $b"; exit 1; }
	echo "  /${e%%=*} $a (new)"
done
python3 -I "$APFS/apfs.py" census "$IMG" | tail -3
echo "BASE census:"
python3 -I "$APFS/apfs.py" census "$BASE" | tail -3
