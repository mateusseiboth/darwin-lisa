#!/bin/sh
# Build HakiOS's Metal plug-in for iOS 26: HakiosMetal.bundle, the class Metal.framework loads when the kernel's
# IOAcceleratorES carries MetalPluginName=HakiosMetal / MetalPluginClassName=HakiosMetalDevice (boot-arg metal=1,
# display/MiniIOMobileFramebuffer.cpp), and metal_test, the program that prints Metal's whole discovery chain.
#
#   PLUGIN=probe (default)  hakios-metal/plugin/HakiosMetal.m: the device is shown to metal_test alone with metal=1
#                           (metal=2 all but backboardd, metal=3 all). The safe one: backboardd keeps drawing in software.
#   PLUGIN=ca               userland/src/HakiosMetalCA.m with HakiosMetalCA-ios26.patch (here) applied to a copy: the
#                           plug-in for the window server; the device is shown to backboardd alone (metal_all=1: all).
#   MESA=1                  with the shader compiler: hakios-metal's AIR -> NIR -> ir3, from ../mesa/mk-mesa-darwin.sh's
#                           archives (MESA_BUILD= says where; default ~/Dev/mobile/userland-26/gpu-build/mesa). Without it
#                           every pipeline is refused by name: the discovery build.
#
# Both over hakios-metal (its C++ core: HakiosGPU's msm DRM requests), from a copy with hakios-metal-*.patch (here)
# applied: hakios-metal-fbslots.patch lets CoreAnimation 26's uber shader find slots for its [[color(n)]] reads. Built on this machine with
# Fedora's clang 22 and ld64.lld against the iPhoneOS 26.5 SDK, plain arm64; the bundle is then relabelled arm64e by
# userland/inject/arm64e-mark.py: backboardd is arm64e and dyld loads only a library of the process's own architecture;
# the code has no pointer authentication and neither has the offline-stripped cache, so only the label changes (as
# libdlisa.dylib). metal_test stays arm64. Nothing of xnu-deps/host/bin is used.
#
#   DEST=DIR      where the bundle goes (default: the sysroot's /System/Library/Extensions)
#   BUNDLE=NAME   the bundle's name (default HakiosMetal). The kernel's accelerator names HakiosMetal with metal=1, or
#                 another with metal_plugin=NAME (gpu-wire.py): the probe one goes in as HakiosMetalProbe beside the ca one
#   DEST_BIN=DIR  where metal_test goes (default: the sysroot's /usr/local/bin)
# Then rebuild the USB image: (cd ~/Dev/mobile/userland-26 && LEAN=1 bash scripts/mkimage-trim.sh)
set -eu
M=/home/mateusseiboth/Dev/mobile
H=$M/research/hackios-xnu-kernel
SDK=$M/research/iOS-SDKs/iPhoneOS26.5.sdk
DEST=${DEST:-$M/userland-26/out/sysroot-26/System/Library/Extensions}
DEST_BIN=${DEST_BIN:-$M/userland-26/out/sysroot-26/usr/local/bin}
PLUGIN=${PLUGIN:-probe}
BUNDLE=${BUNDLE:-HakiosMetal}
MESA=${MESA:-}
MB=${MESA_BUILD:-$M/userland-26/gpu-build/mesa}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d "$SDK" ] || { echo "build-hakios-metal: no $SDK (git sparse checkout of github.com/xybp888/iOS-SDKs)"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
T="-target arm64-apple-ios16.0 -isysroot $SDK"

case "$PLUGIN" in
probe) mkdir -p "$WORK/plugin"
       cp "$H/hakios-metal/plugin/HakiosMetal.m" "$H/hakios-metal/plugin/hakios_metallib.c" "$H/hakios-metal/plugin/hakios_metallib.h" "$WORK/plugin/"
       patch -s -d "$WORK" -p1 < "$HERE/probe-plugin-onedevice.patch"     # one device per process (Metal 26 asserts on two)
       SOURCES="$WORK/plugin/HakiosMetal.m $WORK/plugin/hakios_metallib.c"; INC="-I$WORK/plugin" ;;
ca)    cp "$H/userland/src/HakiosMetalCA.m" "$WORK/HakiosMetalCA.m"
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-ios26.patch"
       # selectors no protocol here declares (the AGX SPIs QuartzCore 26 sends) answered, not raised
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-unknown-selectors.patch"
       # metal_trace=1: per program, the resources each stage declares and the buffers a draw binds
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-trace-binds.patch"
       # the uber shader specialised by the lowest small constant buffer a draw binds; metal_spec_wait=1
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-spec-bound.patch"
       # the "linked" line says how long the link took (a few ms: the stage cache; hundreds: a compile)
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-link-timing.patch"
       # ca_prof_all=1: the ca_prof sampler in every process that loads the plug-in (SpringBoard's main thread)
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-ca-prof-all.patch"
       # a refused render pipeline names its label, functions, descriptor class and the caller's images (metal_all=1)
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-refusal-detail.patch"
       # metal_all=1: the device for the processes that draw (apps, SpringBoard, app extensions), not the daemons; =2 all
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-metal-all-ui.patch"
       # function constants of up to 32 bytes (RenderBox's uint4 _shader_state) handed whole to the compiler
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-wide-constants.patch"
       # the first callers of -newBufferWithBytesNoCopy: and -enqueue, by image and symbol (a ring written after the
       # call, or a reservation of order, is what this plug-in cannot honour; the name says who relies on it)
       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-nocopy-caller.patch"
       SOURCES="$WORK/HakiosMetalCA.m"; INC= ;;
*)     echo "build-hakios-metal: PLUGIN=probe or PLUGIN=ca"; exit 1 ;;
esac

# the compiler: Mesa's -D set as its own ir3 objects were built with (hakios-metal tools/compiler.mk does the same), its
# source and generated include dirs, and the archives
MESA_CXX=
MESA_LIBS=
if [ -n "$MESA" ]; then
	S=$MB/mesa-25.0.7
	B=$MB/build
	[ -f "$B/src/compiler/air/libair.a" ] || { echo "build-hakios-metal: no $B/src/compiler/air/libair.a (run ../mesa/mk-mesa-darwin.sh)"; exit 1; }
	DEFS=$(grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' "$B/build.ninja" | grep -m1 ' ARGS = ' |
		tr ' ' '\n' | grep '^-D' | grep -v "^-D_FORTIFY" | tr '\n' ' ')
	[ -n "$DEFS" ] || { echo "build-hakios-metal: no -D set in $B/build.ninja"; exit 1; }
	MESA_CXX="-DHAKIOS_METAL_MESA $DEFS -isystem $S/src -isystem $S/include -isystem $S/src/freedreno -isystem $S/src/freedreno/common
		-isystem $S/src/compiler -isystem $S/src/compiler/nir -isystem $S/src/compiler/air -isystem $S/src/gallium/include
		-isystem $B/src -isystem $B/src/compiler -isystem $B/src/compiler/nir -isystem $B/src/freedreno
		-isystem $B/src/freedreno/common -isystem $B/src/freedreno/isa"
	for a in src/compiler/air/libair.a src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a \
	    src/freedreno/isa/libir3decode.a src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a \
	    src/freedreno/fdl/libfreedreno_layout.a src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a \
	    src/util/libmesa_util_sse41.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a; do
		MESA_LIBS="$MESA_LIBS $B/$a"
	done
	MESA_LIBS="$MESA_LIBS -lz -lm"
	# the compiled-stage cache's key (hakios-metal src/compiler.cpp: kCacheBuild), from what the compiler is made of
	CACHE=$(cat "$H/hakios-metal/src/compiler.cpp" "$HERE"/hakios-metal-*.patch "$H"/hakios-metal/include/hakios-metal/*.hpp $MB/build/src/compiler/air/libair.a |
		sha256sum | cut -c1-24)
	MESA_CXX="$MESA_CXX -DHAKIOS_METAL_CACHE_BUILD=\"$CACHE\""
fi

# hakios-metal, patched, in the work directory (the HakiOS tree is left as it is)
HMS=$WORK/hakios-metal
mkdir -p "$HMS"
cp -r "$H/hakios-metal/src" "$H/hakios-metal/include" "$H/hakios-metal/tests" "$HMS/"
for p in "$HERE"/hakios-metal-*.patch; do patch -s -d "$HMS" -p1 < "$p"; done

# metal_test, from a copy: it starts its own sub-tests by its own path (metal_test-selfpath.patch; HakiOS had /redwood/bin)
cp "$H/userland/src/metal_test.m" "$WORK/metal_test.m"
patch -s -d "$WORK" -p1 < "$HERE/metal_test-selfpath.patch"

cd "$WORK"
for f in pm4 gpu command compute texture render link compiler metallib c_api fake_gpu iokit_transport; do
	# shellcheck disable=SC2086
	clang++ $T -std=c++17 -O2 -w -Wno-invalid-constexpr -I"$HMS/include" $MESA_CXX \
	    -c "$HMS/src/$f.cpp" -o "$f.o"
done
# the metal2vulkan entry points HakiosMetalCA.m imports weakly (metal_m2v=1): absent in this build
# shellcheck disable=SC2086
clang $T -O1 -w -bundle -fuse-ld=lld -DHAKIOS_METAL_GPU -I"$HMS/include" $INC \
    -framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc -lc++ \
    -Wl,-U,_native_bridge_metal_open -Wl,-U,_native_bridge_metal_first_problem -Wl,-U,_native_bridge_metal_status \
    -Wl,-U,_native_bridge_metal_formats -Wl,-U,_native_bridge_metal_variant_hook \
    -Wl,-adhoc_codesign -o HakiosMetal $SOURCES ./*.o $MESA_LIBS
clang $T -O1 -w -fobjc-arc -Wno-deprecated-declarations -fuse-ld=lld \
    -framework Foundation -framework CoreGraphics -framework CoreImage -framework IOKit -framework Metal \
    -framework OpenGLES -lMobileGestalt -Wl,-adhoc_codesign -o metal_test "$WORK/metal_test.m"

python3 -I "$HERE/../../../../userland/inject/arm64e-mark.py" "$WORK/HakiosMetal"
mkdir -p "$DEST/$BUNDLE.bundle" "$DEST_BIN"
# (the executable keeps its name, which Info.plist's CFBundleExecutable gives; only the bundle's directory changes)
install -m 755 "$WORK/HakiosMetal" "$DEST/$BUNDLE.bundle/HakiosMetal"
install -m 644 "$H/userland/src/HakiosMetal.plist" "$DEST/$BUNDLE.bundle/Info.plist"
install -m 755 "$WORK/metal_test" "$DEST_BIN/metal_test"
echo "$BUNDLE.bundle ($PLUGIN${MESA:+, with the compiler}) -> $DEST/$BUNDLE.bundle; metal_test -> $DEST_BIN/metal_test"
file "$DEST/$BUNDLE.bundle/HakiosMetal" "$DEST_BIN/metal_test" 2>/dev/null || true
