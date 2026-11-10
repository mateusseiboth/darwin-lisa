#!/bin/sh
# Build fbtest (arm64 iOS program, see fbtest.c) and put it in the iOS 26 sysroot as /usr/local/bin/fbtest.
# Runs the compiler in the toolchain container; links with lld (it reads the SDK's .tbd stubs) and signs ad hoc.
# Then rebuild the USB image: (cd ~/Dev/mobile/userland-26 && bash scripts/mkimage-trim.sh)
set -eu
M=/home/mateusseiboth/Dev/mobile
docker run --rm -v $M:/work darwin-lisa/unixnu-toolchain:latest sh -c '
	S=/work/research/hackios-xnu-kernel/xnu-deps/theos/sdks-git/iPhoneOS16.5.sdk
	clang -target arm64-apple-ios16.0 -isysroot $S -O1 -w \
	    --ld-path=/work/research/hackios-xnu-kernel/xnu-deps/host/bin/ld64-lld \
	    -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework IOSurface \
	    -Wl,-adhoc_codesign -o /tmp/fbtest /work/darwin-lisa/userland/tools/fbtest.c &&
	cat /tmp/fbtest' > /tmp/fbtest.$$
install -m 755 /tmp/fbtest.$$ $M/userland-26/out/sysroot-26/usr/local/bin/fbtest
rm -f /tmp/fbtest.$$
echo "fbtest -> userland-26/out/sysroot-26/usr/local/bin/fbtest"
