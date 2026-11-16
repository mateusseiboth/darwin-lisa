#!/bin/sh
# tu-capass for pmOS on lisa (aarch64 musl), the same way as ../build-glfill.sh: clang + ld.lld on this host, the Vulkan
# headers of the Mesa tree, crt and libc from the pmbootstrap apk cache's musl-dev. At run time it dlopens pmOS's own
# turnip (/usr/lib/libvulkan_freedreno.so, mesa-vulkan-freedreno) over the Linux msm driver: the real A642L.
# Output: ~/Dev/mobile/userland-26/out/tu-capass/tu-capass
set -eu
M=$HOME/Dev/mobile
MESA=$M/userland-26/gpu-build/mesa/mesa-25.0.7
APK=$(ls "$HOME"/.local/var/pmbootstrap/cache_apk_aarch64/musl-dev-*.apk | head -1)
C=$HOME/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$M/userland-26/out/tu-capass
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
tar xzf "$APK" -C "$W" 2>/dev/null || true
cp -L "$C/lib/ld-musl-aarch64.so.1" "$W/libc.so"
python3 -I "$HERE/gen-spirv.py" "$W/spirv.h"
mkdir -p "$OUT"
clang --target=aarch64-linux-musl -O2 -nostdlib -fuse-ld=lld -isystem "$MESA/include" -isystem "$W/usr/include" -I"$W" \
	-o "$OUT/tu-capass" "$HERE/tu-capass.c" "$W/usr/lib/crt1.o" "$W/usr/lib/crti.o" -L"$W" -lc "$W/usr/lib/crtn.o" \
	-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1
sha256sum "$OUT/tu-capass"
