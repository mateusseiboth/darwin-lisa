#!/bin/bash
# The Darwin (iOS arm64) build of Mesa's shader compiler for hakios-metal's MESA=1: HakiOS's AIR front end (Metal's
# shader bitcode -> NIR) and ir3 (NIR -> Adreno code) as static archives, linked into HakiosMetal.bundle by
# ../userland/build-hakios-metal.sh MESA=1. The same Mesa as HakiOS's tools/mk-mesa.sh (25.0.7, tools/mesa/*.patch,
# hakios-drm), with darwin-lisa's patches to the front end on top (air-*.patch here).
#
# Built on THIS machine, not in the toolchain container: Fedora's clang 22 and ld64.lld cross-compile for iOS
# directly, so nothing of xnu-deps/host/bin is used or changed (the kernel builds link through its ld64 symlink),
# and nothing here runs as root. Everything lands in $W (default ~/Dev/mobile/userland-26/gpu-build/mesa).
#
# Out: $W/build/src/compiler/air/libair.a and the archives hakios-metal's tools/kernels.mk lists (MESA_ARCHIVE_PATHS),
# $W/mesa-25.0.7 (patched sources, for the headers), $W/build/build.ninja (the -D set the driver must compile with).
# Needs: meson, ninja, bison, flex, python3 with mako/pyyaml/packaging, clang, ld64.lld, llvm-ar.
set -euo pipefail
M=/home/mateusseiboth/Dev/mobile
H=$M/research/hackios-xnu-kernel
HERE=$(cd "$(dirname "$0")" && pwd)
W=${W:-$M/userland-26/gpu-build/mesa}
SDK=${SDK:-$H/xnu-deps/theos/sdks-git/iPhoneOS16.5.sdk}
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
TARBALL=$H/xnu-deps/build/mesa/mesa-$VERSION.tar.xz
mkdir -p "$W"
cd "$W"

# sources: the archive, HakiOS's patches, hakios-drm, the AIR front end with ours on top. A change to any of them
# starts the tree and the build again (a build half from the old front end is the worst case).
STAMP=$(cat "$H"/tools/mesa/*.patch "$HERE"/air-*.patch "$H"/tools/mesa/air/* "$H"/tools/mesa/hakios-drm/* | sha256sum | cut -c1-64)
if [ "$(cat mesa-$VERSION/.darwin-lisa 2>/dev/null)" != "$STAMP" ]; then
	[ -f mesa-$VERSION.tar.xz ] || cp "$TARBALL" . 2>/dev/null ||
		curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build
	tar xJf mesa-$VERSION.tar.xz
	for p in "$H"/tools/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	cp -r "$H/tools/mesa/hakios-drm" mesa-$VERSION/src/hakios-drm
	rm -rf mesa-$VERSION/src/compiler/air
	cp -r "$H/tools/mesa/air" mesa-$VERSION/src/compiler/air
	for p in "$HERE"/air-*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION/src/compiler/air -p1 < "$p"; done
	echo "$STAMP" > mesa-$VERSION/.darwin-lisa
	echo "mk-mesa-darwin: sources patched ($(ls "$H"/tools/mesa/*.patch "$HERE"/air-*.patch | wc -l) patches)"
fi

T="'-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK'"
cat > cross.ini <<INI
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
INI
# zlib is a .tbd in the SDK, which meson's library search does not take: a .pc for it (as mk-mesa.sh)
mkdir -p pkgconfig
cat > pkgconfig/zlib.pc <<PC
prefix=$SDK/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib (SDK stub)
Version: 1.2.11
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
PC
export PKG_CONFIG_LIBDIR=$W/pkgconfig
unset PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_PATH

[ -f build/build.ninja ] || meson setup build mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled \
	-Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log

ARCHIVES="src/compiler/air/libair.a src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a
	src/freedreno/isa/libir3decode.a src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
	src/freedreno/fdl/libfreedreno_layout.a src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a
	src/util/libmesa_util_sse41.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a"
ninja -C build $ARCHIVES > ninja.log
for a in $ARCHIVES; do
	[ -f "build/$a" ] || { echo "mk-mesa-darwin: build/$a missing"; exit 1; }
done
echo "mk-mesa-darwin: $(echo $ARCHIVES | wc -w) archives in $W/build ($(du -ch $(for a in $ARCHIVES; do echo build/$a; done) | tail -1 | cut -f1))"
