#!/bin/sh
# Build dirtest (dirtest.c) for the phone: plain arm64 against the iPhoneOS 26.5 SDK with this machine's clang and
# ld64.lld (as userland/build-hakios-metal.sh builds metal_test), ad-hoc signed, into the sysroot's /usr/local/bin.
set -eu
M=/home/mateusseiboth/Dev/mobile
SDK=$M/research/iOS-SDKs/iPhoneOS26.5.sdk
DEST=${DEST:-$M/userland-26/out/sysroot-26/usr/local/bin}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d "$SDK" ] || { echo "build-dirtest: no $SDK"; exit 1; }
mkdir -p "$DEST"
clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O1 -Wall -fuse-ld=lld -Wl,-adhoc_codesign -o "$DEST/dirtest" "$HERE/dirtest.c"
file "$DEST/dirtest"
