#!/bin/sh
# Build tools/glfill.c for pmOS (aarch64 musl) on this host: clang + ld.lld, Mesa's EGL/GLES headers from the host
# Mesa tree, crt and libc from the pmbootstrap apk cache's musl-dev, libEGL/libGLESv2 from the lisa chroot.
# Output: ~/Dev/mobile/userland-26/out/glfill/glfill (darwin-lisa session 13).
set -eu
M=$HOME/Dev/mobile
C=$HOME/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa
MESA=$M/research/hackios-xnu-kernel/build/mesa-host/mesa-25.0.7
APK=$(ls $HOME/.local/var/pmbootstrap/cache_apk_aarch64/musl-dev-*.apk | head -1)
OUT=$M/userland-26/out/glfill
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
tar xzf "$APK" -C "$W" 2>/dev/null || true
cp -L "$C/usr/lib/libEGL.so.1" "$W/libEGL.so"
cp -L "$C/usr/lib/libGLESv2.so.2" "$W/libGLESv2.so"
cp -L "$C/lib/ld-musl-aarch64.so.1" "$W/libc.so"
mkdir -p "$OUT"
clang --target=aarch64-linux-musl -O2 -nostdlib -fuse-ld=lld -isystem "$MESA/include" -isystem "$W/usr/include" \
	-o "$OUT/glfill" "$(dirname "$0")/glfill.c" "$W/usr/lib/crt1.o" "$W/usr/lib/crti.o" -L"$W" -lEGL -lGLESv2 -lc \
	"$W/usr/lib/crtn.o" -Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1 -Wl,--allow-shlib-undefined
sha256sum "$OUT/glfill"
