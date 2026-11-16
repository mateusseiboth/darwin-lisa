#!/bin/sh
# Turnip for this host (x86_64 Linux) over Mesa's drm-shim, from the same tree as the phone's (fetch-src.sh: Mesa 85fda3b + dada's
# patch + mesa-lisa.patch, whose freedreno_noop.c row 635 is the Adreno 642L: chip 0x06030500, 512 KB of GMEM). Nothing runs on a
# GPU, but every pipeline is compiled to ir3 as on the phone: m2v-coverage (T0b) and tu-capass use it.
#   build-host-turnip.sh -> $UNIXNU/build/host/src/freedreno/vulkan/libvulkan_freedreno.so, .../drm-shim/libfreedreno_noop_drm_shim.so,
#                           $UNIXNU/build/host/turnip-icd.json (VK_DRIVER_FILES for the system loader)
# libdrm's headers: libdrm 2.4.134 from dri.freedesktop.org (the host has the library, not its headers).
set -eu
UNIXNU=${UNIXNU:-$HOME/Dev/mobile/userland-26/gpu-build/unixnu}
D=$UNIXNU/host-deps/libdrm
if [ ! -f "$D/pc/libdrm.pc" ]; then
	mkdir -p "$D/pc"
	curl -sSfL https://dri.freedesktop.org/libdrm/libdrm-2.4.134.tar.xz | tar xJ -C "$D"
	cat > "$D/pc/libdrm.pc" <<EOF
prefix=$D/libdrm-2.4.134
includedir=\${prefix}/include/drm
Name: libdrm
Version: 2.4.134
Description: libdrm headers (host build) over the system libdrm.so.2
Libs: /usr/lib64/libdrm.so.2
Cflags: -I\${includedir} -I\${prefix}
EOF
fi
export PATH=$UNIXNU/tools:$PATH PKG_CONFIG_PATH=$D/pc
B=$UNIXNU/build/host
[ -f "$B/build.ninja" ] || meson setup "$B" "$UNIXNU/src/mesa" --buildtype=release -Db_ndebug=true -Dplatforms= -Dgallium-drivers= \
	-Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dtools=drm-shim -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
	-Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dvideo-codecs= -Dlmsensors=disabled -Dbuild-tests=false -Dperfetto=false -Dvulkan-layers= --wrap-mode=nodownload
ninja -C "$B" src/freedreno/vulkan/libvulkan_freedreno.so src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so
cat > "$B/turnip-icd.json" <<EOF
{"file_format_version": "1.0.1", "ICD": {"library_path": "$B/src/freedreno/vulkan/libvulkan_freedreno.so", "api_version": "1.4.0"}}
EOF
echo "LD_PRELOAD=$B/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so FD_GPU_ID=635 VK_DRIVER_FILES=$B/turnip-icd.json"
