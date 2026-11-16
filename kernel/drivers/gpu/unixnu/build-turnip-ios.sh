#!/bin/sh
# Turnip (Mesa freedreno Vulkan) with UniXNU's Darwin kernel backend, for lisa's iOS 26 (T1 of docs/gpu-driver-rewrite.md):
# dada's gpu2/build_turnip_ios.sh moved from Xcode on macOS to this host's clang 22 + ld64.lld and the iPhoneOS 26.5 SDK, plain
# arm64 (relabelled arm64e by the plug-in's build, as HakiosMetal is). Sources: $UNIXNU/src/mesa (fetch-src.sh: Mesa 85fda3b with
# dada's mesa-85fda3b-dada.patch, then mesa-lisa.patch here), $UNIXNU/src/nativeadreno (NativeAdrenoABI.h).
#   build-turnip-ios.sh   -> $UNIXNU/out/turnip/libvulkan_freedreno.dylib
set -eu
M=$HOME/Dev/mobile
UNIXNU=${UNIXNU:-$M/userland-26/gpu-build/unixnu}
SDK=${SDK:-$M/research/iOS-SDKs/iPhoneOS26.5.sdk}
MESA=$UNIXNU/src/mesa
ABI=$UNIXNU/src/nativeadreno
B=$UNIXNU/build/turnip-ios
OUT=$UNIXNU/out/turnip
[ -d "$SDK" ] || { echo "build-turnip-ios: no $SDK"; exit 1; }
[ -f "$ABI/NativeAdrenoABI.h" ] || { echo "build-turnip-ios: no $ABI/NativeAdrenoABI.h (fetch-src.sh)"; exit 1; }
mkdir -p "$B/pc" "$OUT"
export PATH=$UNIXNU/tools:$PATH                 # glslangValidator (host-deps/glslang, built by build-host-tools.sh)
T="'-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK'"
cat > "$B/ios.cross" <<EOF
[binaries]
c = ['clang', $T]
cpp = ['clang++', $T, '-fuse-cxa-atexit']
objc = ['clang', $T]
c_ld = 'lld'
cpp_ld = 'lld'
objc_ld = 'lld'
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_link_args = ['-Wl,-adhoc_codesign']
cpp_link_args = ['-Wl,-adhoc_codesign']
EOF
cat > "$B/pc/zlib.pc" <<EOF
prefix=$SDK/usr
Name: zlib
Description: zlib (the SDK's stub)
Version: 1.2.12
Libs: -L\${prefix}/lib -lz
Cflags: -I\${prefix}/include
EOF
# a meson re-run must keep pkg-config at the SDK's .pc files, or it links the host's libraries
export PKG_CONFIG_LIBDIR=$B/pc PKG_CONFIG_PATH=
# (an ad-hoc signature: arm64e-mark.py re-hashes it after the relabel, as for HakiosMetal)
if [ ! -f "$B/tu/build.ninja" ]; then
	meson setup "$B/tu" "$MESA" --cross-file "$B/ios.cross" --wrap-mode=nodownload \
		-Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=darwin "-Dtu-darwin-abi-dir=$ABI" \
		-Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
		-Dllvm=disabled -Dxmlconfig=disabled -Dshader-cache=enabled -Dtools= -Dbuild-tests=false -Dvideo-codecs= \
		-Dvulkan-layers= -Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled -Dexpat=disabled -Dzstd=disabled \
		-Dperfetto=false -Dbuildtype=release -Db_ndebug=true -Dtu-builtin-source-id=lisa -Dtu-builtin-pack-header=
fi
# the disk cache's identity of this Turnip: its sources (commit and local changes), not the library's mtime (dada's reason)
BUILD_ID=$( (cd "$MESA" && git rev-parse HEAD && git diff) | sha256sum | cut -c1-40)
grep -q "TU_BUILD_ID_OVERRIDE=\"$BUILD_ID\"" "$B/tu/build.ninja" 2>/dev/null || meson configure "$B/tu" -Dtu-build-id="$BUILD_ID" >/dev/null
echo "turnip build id $BUILD_ID"
ninja -C "$B/tu" src/freedreno/vulkan/libvulkan_freedreno.dylib
LIB=$OUT/libvulkan_freedreno.dylib
cp "$B/tu/src/freedreno/vulkan/libvulkan_freedreno.dylib" "$LIB"
llvm-install-name-tool -id /usr/lib/libvulkan_freedreno.dylib "$LIB"
llvm-strip -x "$LIB"
llvm-nm -gU "$LIB" | grep -q ' T _vk_icdGetInstanceProcAddr' || { echo "build-turnip-ios: no vk_icdGetInstanceProcAddr"; exit 1; }
llvm-nm -gU "$LIB" | grep -q ' T _tu_darwin_note_surface' || { echo "build-turnip-ios: no tu_darwin_note_surface"; exit 1; }
ls -l "$LIB"
sha256sum "$LIB"
