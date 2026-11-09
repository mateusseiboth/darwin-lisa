#!/bin/bash
# Build the iOS-26 first-boot init (static arm64 Mach-O, LC_UNIXTHREAD, no dyld).
# Runs inside darwin-lisa/unixnu-toolchain:latest. Produces /work/out/launchd.
set -eu
B=/opt/unixnu-linux/CommandLineTools/usr/bin
CC="clang --target=arm64-apple-ios16.0 -arch arm64 -Os -static -nostdlib -nostdlibinc -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="--ld-path=$B/ld -Wl,-e,_start -Wl,-static -Wl,-pagezero_size,0x100000000"
cd /work
mkdir -p out obj
$CC -I. -c mini.c -o obj/mini.o
$CC -c start.S -o obj/start.o
$CC -I. $LD -o out/launchd init-26.c obj/mini.o obj/start.o
$B/strip -no_uuid out/launchd 2>/dev/null || true
echo "built out/launchd"
