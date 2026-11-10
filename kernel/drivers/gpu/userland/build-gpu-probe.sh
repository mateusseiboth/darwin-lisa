#!/bin/sh
# Build hakios-metal-probe (HakiOS's hakios-metal/tools/probe.cpp) and gputest (gputest.c) for iOS 26 and put them in
# the iOS 26 sysroot as /usr/local/bin/hakios-metal-probe and /usr/local/bin/gputest. The probe is the GPU test that comes before any Metal or CoreAnimation. It opens the kernel's
# HakiosGPU user client, copies and fills buffers with the command processor, runs compute kernels and draws the first
# triangles into a 64x64 target, every result checked word by word ("probe: ok ..." / "probe: FAIL ...").
#
# No Mesa in this build: the kernels and the triangle shaders are the precompiled ir3 fixtures
# (hakios-metal/tests/fixtures/*_air.hpp); the stages that compile on the phone say "no compiler in this build".
# Plain arm64 against the iPhoneOS 26.5 SDK (a plain arm64 program runs on this port: the kernel puts dyld through the
# PAC-stripping pager), linked with lld (the SDK's stubs are .tbd), signed ad hoc. Runs the compiler in the toolchain
# container, like userland/tools/build-fbtest.sh.
#
#   DEST=DIR  install there instead of the sysroot (a dry run: nothing of the image changes)
# Then rebuild the USB image: (cd ~/Dev/mobile/userland-26 && LEAN=1 bash scripts/mkimage-trim.sh)
set -eu
M=/home/mateusseiboth/Dev/mobile
SDK26=research/iOS-SDKs/iPhoneOS26.5.sdk
DEST=${DEST:-$M/userland-26/out/sysroot-26/usr/local/bin}
[ -d "$M/$SDK26" ] || { echo "build-gpu-probe: no $M/$SDK26 (git sparse checkout of github.com/xybp888/iOS-SDKs)"; exit 1; }
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
docker run --rm -v $M:/work:ro darwin-lisa/unixnu-toolchain:latest sh -c '
	set -e
	S=/work/'"$SDK26"'
	H=/work/research/hackios-xnu-kernel/hakios-metal
	# the 26.5 SDK libc++ headers want builtins newer than the container clang 18 has: the C++ headers are the
	# 16.5 SDK ones (libc++ keeps its ABI), everything else (frameworks, .tbd stubs) is the 26.5 SDK
	CXX16=/work/research/hackios-xnu-kernel/xnu-deps/theos/sdks-git/iPhoneOS16.5.sdk/usr/include/c++/v1
	cd $H
	clang++ -target arm64-apple-ios16.0 -isysroot $S -nostdinc++ -isystem $CXX16 -std=c++17 -O2 -w \
	    -Wno-invalid-constexpr -Iinclude \
	    --ld-path=/work/research/hackios-xnu-kernel/xnu-deps/host/bin/ld64-lld \
	    src/pm4.cpp src/gpu.cpp src/command.cpp src/compute.cpp src/texture.cpp src/render.cpp src/link.cpp \
	    src/compiler.cpp src/metallib.cpp src/c_api.cpp src/fake_gpu.cpp src/iokit_transport.cpp tools/probe.cpp \
	    -framework IOKit -framework CoreFoundation -framework IOSurface -Wl,-adhoc_codesign \
	    -o /tmp/hakios-metal-probe >&2 &&
	cat /tmp/hakios-metal-probe' > "$OUT"
mkdir -p "$DEST"
install -m 755 "$OUT" "$DEST/hakios-metal-probe"
echo "hakios-metal-probe -> $DEST/hakios-metal-probe"
# gputest (gputest.c here): the probe and metal_test in order under one init_run= entry. Fedora's clang 22 builds it here.
HERE=$(cd "$(dirname "$0")" && pwd)
clang -target arm64-apple-ios16.0 -isysroot "$M/$SDK26" -O1 -Wall -fuse-ld=lld -Wl,-adhoc_codesign \
    -o "$OUT.gputest" "$HERE/gputest.c"
install -m 755 "$OUT.gputest" "$DEST/gputest"
rm -f "$OUT.gputest"
echo "gputest -> $DEST/gputest"
file "$DEST/hakios-metal-probe" "$DEST/gputest" 2>/dev/null || true
