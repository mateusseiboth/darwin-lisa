#!/bin/sh
# AdrenoMetal.bundle for lisa's iOS 26 (T1 of docs/gpu-driver-rewrite.md): UniXNU's AdrenoMetal plug-in (c6d29ca with dada's
# nativemetal-dada.patch and nativemetal-lisa.patch here) over the metal2vulkan bridge (build-bridge.sh) and Turnip with the Darwin
# backend (build-turnip-ios.sh), which goes inside the bundle next to the executable (turnip::libraryPath). dada's
# gpu2/build_plugin.sh moved to this host's clang 22 + ld64.lld and the iPhoneOS 26.5 SDK.
#
# The kernel's accelerator names the plug-in by boot-arg (gpu-wire.py step 6): metal=1 metal_plugin=AdrenoMetal. Its class name
# stays HakiosMetalDevice, which this bundle does not have, so Metal takes the bundle's NSPrincipalClass (NativeAdrenoMetalDevice).
# Without the boot-arg the accelerator names HakiosMetal, today's driver: that is the rollback.
# Which processes get a device: NativeAdrenoMetal.clients beside the bundle ("*": every one), or boot-arg m2v_clients=a,b.
#
# Built plain arm64 and relabelled arm64e (userland/inject/arm64e-mark.py), executable and Turnip alike: backboardd and the apps
# are arm64e, dyld loads only libraries of the process's architecture, and this userland has no pointer authentication.
# metal2vulkan (in the executable) is GPL-2.0-or-later OR LGPL-3.0-or-later: its source goes with the bundle
# (docs/gpu-driver-rewrite.md, "Licences").
#
#   build-plugin.sh   -> $UNIXNU/out/plugin/AdrenoMetal.bundle, $UNIXNU/out/plugin/NativeAdrenoMetal.clients
set -eu
M=$HOME/Dev/mobile
UNIXNU=${UNIXNU:-$M/userland-26/gpu-build/unixnu}
SDK=${SDK:-$M/research/iOS-SDKs/iPhoneOS26.5.sdk}
HERE=$(cd "$(dirname "$0")" && pwd)
MARK=$HERE/../../../../userland/inject/arm64e-mark.py
NM=$UNIXNU/src/adrenometal
NA=$UNIXNU/src/nativeadreno
PM4=$UNIXNU/src/adrenopm4
MESA=$UNIXNU/src/mesa
OUT=$UNIXNU/out/plugin
B=$UNIXNU/build/plugin
BRIDGE=$UNIXNU/out/bridge/ios/libnative_metal_bridge.a
TURNIP=$UNIXNU/out/turnip/libvulkan_freedreno.dylib
export PATH=$UNIXNU/tools:$PATH
for f in "$BRIDGE" "$TURNIP" "$MARK"; do [ -f "$f" ] || { echo "build-plugin: missing $f"; exit 1; }; done
mkdir -p "$OUT" "$B/gen"
T="-target arm64-apple-ios16.0 -isysroot $SDK"

# the plug-in's smoke shaders as SPIR-V words (glslangValidator from build-host-tools.sh)
{
	echo '// Generated from NativeMetal/shaders by build-plugin.sh; do not edit.'
	echo '#pragma once'
	echo '#include <stdint.h>'
	for s in "$NM"/shaders/smoke_*; do
		n=$(basename "$s")
		glslangValidator -V --target-env vulkan1.1 -o "$B/$n.spv" "$s" >/dev/null
		python3 -I -c 'import struct,sys; d=open(sys.argv[1],"rb").read(); w=struct.unpack("<%dI" % (len(d)//4), d)
print("static const uint32_t kSpv_%s[] = {" % sys.argv[2].replace(".", "_"))
for i in range(0, len(w), 8): print("    " + ", ".join("0x%08x" % x for x in w[i:i+8]) + ",")
print("};")' "$B/$n.spv" "$n"
	done
} > "$B/gen/NativeTurnipShaders.h"

BUNDLE=$OUT/AdrenoMetal.bundle
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
# shellcheck disable=SC2086
clang $T -O2 -std=c11 -c "$PM4/AdrenoPM4.c" -o "$B/AdrenoPM4.o"
# shellcheck disable=SC2086
clang++ $T -O2 -std=c++17 -fobjc-arc -w -bundle -fuse-ld=lld \
	-I "$NM" -I "$NA" -I "$PM4" -I "$MESA/include" -I "$B/gen" \
	-x objective-c++ "$NM/NativeAdrenoMetalPlugin.mm" "$NM/NativeBridgeMetalFrontend.mm" -x none \
	"$B/AdrenoPM4.o" "$BRIDGE" -Wl,-dead_strip -lobjc -lc++ -framework Foundation -framework Metal -framework IOSurface \
	-framework CoreFoundation -framework IOKit -Wl,-adhoc_codesign -o "$BUNDLE/AdrenoMetal"
llvm-strip -x "$BUNDLE/AdrenoMetal"
python3 -I "$MARK" "$BUNDLE/AdrenoMetal"
cp "$TURNIP" "$BUNDLE/libvulkan_freedreno.dylib"
python3 -I "$MARK" "$BUNDLE/libvulkan_freedreno.dylib"
cat > "$BUNDLE/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>com.apple.AdrenoMetal</string>
	<key>CFBundleExecutable</key>
	<string>AdrenoMetal</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleVersion</key>
	<string>1</string>
	<key>CFBundleShortVersionString</key>
	<string>1</string>
	<key>NSPrincipalClass</key>
	<string>NativeAdrenoMetalDevice</string>
	<key>MinimumOSVersion</key>
	<string>16.0</string>
</dict>
</plist>
EOF
echo "*" > "$OUT/NativeAdrenoMetal.clients"
llvm-objdump --macho --dylibs-used "$BUNDLE/AdrenoMetal" | head -14
ls -l "$BUNDLE"
sha256sum "$BUNDLE/AdrenoMetal" "$BUNDLE/libvulkan_freedreno.dylib"
