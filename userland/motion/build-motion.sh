#!/bin/sh
# Build libdlisa-motion.dylib (dlisa-motion.c: backboardd's device orientation from the kernel's sensors) into
# darwin-lisa/userland/motion/out/. Nothing is installed: kernel/drivers/adsp/install-root.sh SYSROOT does that.
# Same toolchain and steps as userland/inject/build-inject.sh: the iOS 16.5 SDK's headers, ld64-lld, ad-hoc signed,
# then arm64e-mark.py (dyld inserts only an arm64e library into the arm64e backboardd).
set -eu
M=/home/mateusseiboth/Dev/mobile
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/out"
docker run --rm -v $M:$M darwin-lisa/unixnu-toolchain:latest sh -c '
	S='"$M"'/research/hackios-xnu-kernel/xnu-deps/theos/sdks-git/iPhoneOS16.5.sdk
	clang -target arm64-apple-ios16.0 -isysroot $S -O1 -Wall -fblocks -dynamiclib \
	    --ld-path='"$M"'/research/hackios-xnu-kernel/xnu-deps/host/bin/ld64-lld \
	    -install_name /usr/local/lib/libdlisa-motion.dylib -lobjc \
	    -Wl,-fixup_chains -Wl,-adhoc_codesign -o /tmp/libdlisa-motion.dylib '"$HERE"'/dlisa-motion.c >&2 &&
	cat /tmp/libdlisa-motion.dylib' > "$HERE/out/libdlisa-motion.dylib.tmp"
python3 -I "$M/darwin-lisa/userland/inject/arm64e-mark.py" "$HERE/out/libdlisa-motion.dylib.tmp"
mv "$HERE/out/libdlisa-motion.dylib.tmp" "$HERE/out/libdlisa-motion.dylib"
echo "built $HERE/out/libdlisa-motion.dylib"
