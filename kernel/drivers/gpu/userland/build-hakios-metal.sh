#!/bin/sh
# Build HakiOS's Metal plug-in for iOS 26: HakiosMetal.bundle, the class Metal.framework loads when the kernel's
# IOAcceleratorES carries MetalPluginName=HakiosMetal / MetalPluginClassName=HakiosMetalDevice (boot-arg metal=1,
# display/MiniIOMobileFramebuffer.cpp), and metal_test, the program that prints Metal's whole discovery chain.
#
#   PLUGIN=probe (default)  hakios-metal/plugin/HakiosMetal.m: the device is shown to metal_test alone with metal=1
#                           (metal=2 all but backboardd, metal=3 all). The safe one: backboardd keeps drawing in software.
#   PLUGIN=ca               userland/src/HakiosMetalCA.m, the plug-in that served iOS 16.5's window server: the device
#                           is shown to backboardd alone (metal_all=1: everyone). Only with the compiler (below).
#
# Both over hakios-metal (its C++ core: HakiosGPU's msm DRM requests), unchanged from HakiOS.
#   - plain arm64 against the iPhoneOS 26.5 SDK (libc++ headers from the 16.5 SDK: the 26.5 ones want a newer clang than
#     the container's 18), then the bundle is relabelled arm64e by userland/inject/arm64e-mark.py: backboardd is arm64e
#     and dyld loads only a library of the process's own architecture; the code has no pointer authentication and
#     neither has the offline-stripped cache, so only the label changes (as libdlisa.dylib). metal_test stays arm64.
#   - WITHOUT Mesa: hakios-metal's AIR -> NIR -> ir3 compiler is not in it (tools/mk-mesa.sh's Darwin build of Mesa
#     with HakiOS's AIR front end, then `make ios MESA=1` in hakios-metal), so every pipeline is refused by name. This is
#     the discovery build: Metal finds the device, MTLAddDevice accepts it, queue, buffers, blits and fills on the GPU.
#
#   DEST=DIR      where the bundle goes (default: the sysroot's /System/Library/Extensions)
#   DEST_BIN=DIR  where metal_test goes (default: the sysroot's /usr/local/bin)
# Then rebuild the USB image: (cd ~/Dev/mobile/userland-26 && LEAN=1 bash scripts/mkimage-trim.sh)
set -eu
M=/home/mateusseiboth/Dev/mobile
SDK26=research/iOS-SDKs/iPhoneOS26.5.sdk
DEST=${DEST:-$M/userland-26/out/sysroot-26/System/Library/Extensions}
DEST_BIN=${DEST_BIN:-$M/userland-26/out/sysroot-26/usr/local/bin}
PLUGIN=${PLUGIN:-probe}
HERE=$(cd "$(dirname "$0")" && pwd)
case "$PLUGIN" in
probe) SOURCES='$H/hakios-metal/plugin/HakiosMetal.m $H/hakios-metal/plugin/hakios_metallib.c'; INC='-I$H/hakios-metal/plugin' ;;
ca)    SOURCES='$H/userland/src/HakiosMetalCA.m'; INC= ;;
*)     echo "build-hakios-metal: PLUGIN=probe or PLUGIN=ca"; exit 1 ;;
esac
[ -d "$M/$SDK26" ] || { echo "build-hakios-metal: no $M/$SDK26 (git sparse checkout of github.com/xybp888/iOS-SDKs)"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
docker run --rm -v $M:/work:ro darwin-lisa/unixnu-toolchain:latest sh -c '
	set -e
	S=/work/'"$SDK26"'
	H=/work/research/hackios-xnu-kernel
	CXX16=$H/xnu-deps/theos/sdks-git/iPhoneOS16.5.sdk/usr/include/c++/v1
	LLD=$H/xnu-deps/host/bin/ld64-lld
	T="-target arm64-apple-ios16.0 -isysroot $S"
	mkdir -p /tmp/hm && cd /tmp/hm
	for f in pm4 gpu command compute texture render link compiler metallib c_api fake_gpu iokit_transport; do
		clang++ $T -nostdinc++ -isystem $CXX16 -std=c++17 -O2 -w -Wno-invalid-constexpr -I$H/hakios-metal/include \
		    -c $H/hakios-metal/src/$f.cpp -o $f.o >&2
	done
	# the metal2vulkan entry points HakiosMetalCA.m imports weakly (metal_m2v=1): absent in this build
	clang $T -O1 -w -bundle --ld-path=$LLD -nostdinc++ -isystem $CXX16 -DHAKIOS_METAL_GPU -I$H/hakios-metal/include '"$INC"' \
	    -framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc -lc++ \
	    -Wl,-U,_native_bridge_metal_open -Wl,-U,_native_bridge_metal_first_problem -Wl,-U,_native_bridge_metal_status \
	    -Wl,-U,_native_bridge_metal_formats -Wl,-U,_native_bridge_metal_variant_hook \
	    -Wl,-adhoc_codesign -o /tmp/hm/HakiosMetal '"$SOURCES"' *.o >&2
	clang $T -O1 -w -fobjc-arc -Wno-deprecated-declarations --ld-path=$LLD \
	    -framework Foundation -framework CoreGraphics -framework CoreImage -framework IOKit -framework Metal \
	    -framework OpenGLES -lMobileGestalt -Wl,-adhoc_codesign -o /tmp/hm/metal_test $H/userland/src/metal_test.m >&2
	tar -C /tmp/hm -cf - HakiosMetal metal_test' | tar -C "$WORK" -xf -
python3 -I "$HERE/../../../../userland/inject/arm64e-mark.py" "$WORK/HakiosMetal"
mkdir -p "$DEST/HakiosMetal.bundle" "$DEST_BIN"
install -m 755 "$WORK/HakiosMetal" "$DEST/HakiosMetal.bundle/HakiosMetal"
install -m 644 "$M/research/hackios-xnu-kernel/userland/src/HakiosMetal.plist" "$DEST/HakiosMetal.bundle/Info.plist"
install -m 755 "$WORK/metal_test" "$DEST_BIN/metal_test"
echo "HakiosMetal.bundle ($PLUGIN) -> $DEST/HakiosMetal.bundle; metal_test -> $DEST_BIN/metal_test"
file "$DEST/HakiosMetal.bundle/HakiosMetal" "$DEST_BIN/metal_test" 2>/dev/null || true
