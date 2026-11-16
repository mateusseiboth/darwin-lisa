#!/bin/sh
# Build libdlisa.dylib (dlisa-inject.c) and put it in the iOS 26 sysroot as /usr/local/lib/libdlisa.dylib, with
# /etc/dlisa-inject.conf (which programs get it; init26 hands the file to kern.exec_inject).
# arm64e: dyld only inserts a library of the process's own architecture, and the stock daemons are arm64e. lld
# labels the output arm64 ALL, so arm64e-mark.py sets the arm64e cpusubtype and re-hashes the ad-hoc signature.
# The code has no pointer authentication in it; the kernel and the offline-stripped cache have none either.
# The conf also names libdlisa-motion.dylib (backboardd: rotation and the sensors' HID services, userland/motion/),
# so that is built if needed and installed too: dyld ends a process whose inserted library is missing, and an
# earlier version of this script dropped the motion line by rewriting the conf. Every library the conf names is
# checked to be in the root before the conf goes in.
# SR=<root> installs into another root (a scratch copy); default the shared sysroot.
# Then rebuild the USB image: (cd ~/Dev/mobile/userland-26 && LEAN=1 bash scripts/mkimage-trim.sh)
set -eu
M=/home/mateusseiboth/Dev/mobile
SR=${SR:-$M/userland-26/out/sysroot-26}
HERE=$(cd "$(dirname "$0")" && pwd)
docker run --rm -v $M:/work darwin-lisa/unixnu-toolchain:latest sh -c '
	S=/work/research/hackios-xnu-kernel/xnu-deps/theos/sdks-git/iPhoneOS16.5.sdk
	clang -target arm64-apple-ios16.0 -isysroot $S -O1 -Wall -dynamiclib \
	    --ld-path=/work/research/hackios-xnu-kernel/xnu-deps/host/bin/ld64-lld \
	    -install_name /usr/local/lib/libdlisa.dylib -lobjc -framework CoreFoundation \
	    -Wl,-fixup_chains -Wl,-adhoc_codesign -o /tmp/libdlisa.dylib /work/darwin-lisa/userland/inject/dlisa-inject.c >&2 &&
	cat /tmp/libdlisa.dylib' > /tmp/libdlisa.$$
mkdir -p $SR/usr/local/lib
python3 "$HERE/arm64e-mark.py" /tmp/libdlisa.$$
install -m 755 /tmp/libdlisa.$$ $SR/usr/local/lib/libdlisa.dylib
rm -f /tmp/libdlisa.$$
MOTION=$HERE/../motion
[ -f "$MOTION/out/libdlisa-motion.dylib" ] || sh "$MOTION/build-motion.sh"
install -m 755 "$MOTION/out/libdlisa-motion.dylib" $SR/usr/local/lib/libdlisa-motion.dylib
for library in $(awk '{ print $1 }' "$HERE/dlisa-inject.conf"); do
	[ -f "$SR$library" ] || { echo "dlisa-inject.conf names $library, which is not in $SR: conf not installed" >&2; exit 1; }
done
mkdir -p $SR/private/etc
install -m 644 "$HERE/dlisa-inject.conf" $SR/private/etc/dlisa-inject.conf
echo "libdlisa.dylib, libdlisa-motion.dylib, dlisa-inject.conf -> $SR"
file $SR/usr/local/lib/libdlisa.dylib 2>/dev/null || true
