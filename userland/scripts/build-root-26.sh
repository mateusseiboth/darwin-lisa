#!/bin/bash
# Build the iOS-26 first-boot init and the SMALL root-26.apfs boot image.
#
#   build-root-26.sh            build the init, then root-26.apfs, then verify
#   build-root-26.sh init-only  only build the init (out/launchd); used by build-system-26.sh
#
# The init is a static arm64 Mach-O (LC_UNIXTHREAD, no dyld), built in the
# darwin-lisa/unixnu-toolchain image with clang 18 + the cctools `ld` it ships
# (ld64.lld cannot make a -static Mach-O: it emits LC_MAIN + a /usr/lib/dyld
# load command, which a kernel-exec'd init must not have). root-26.apfs is an
# APFS container made by HakiOS's mkapfs.py, so the port's APFS driver reads it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$(cd "$HERE/.." && pwd)"
HK=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel
IMAGE="${TOOLCHAIN_IMAGE:-darwin-lisa/unixnu-toolchain:latest}"
INIT="$BASE/init"
OUT="$BASE/out"
mkdir -p "$OUT"

# --- the init (inside docker; the SDK is not needed, libmini talks to the kernel) ---
docker run --rm -v "$INIT":/work -w /work "$IMAGE" bash /work/build-init.sh
install -m 0755 "$INIT/out/launchd" "$OUT/launchd"
if /home/mateusseiboth/Dev/mobile/tools/ipsw-dl/ipsw macho info "$OUT/launchd" 2>/dev/null | grep -q LC_LOAD_DYLINKER; then
	echo "init has a /usr/lib/dyld load command; a kernel-exec'd init must be LC_UNIXTHREAD only" >&2; exit 1
fi
[ "${1:-}" = init-only ] && { echo "init built: $OUT/launchd"; exit 0; }

# --- the minimal bootable-shaped root tree ---
R="$OUT/root-min"
rm -rf "$R"
mkdir -p "$R"/{sbin,dev,etc,bin,usr/lib,usr/bin,System/Library,Library,Applications,private/var,private/tmp,private/etc}
ln -sfn private/var "$R/var"
ln -sfn private/tmp "$R/tmp"
install -m 0755 "$OUT/launchd" "$R/sbin/launchd"
printf 'HakiOS iOS 26 port (lisa / SM7325)\nbuild 23F84, userland-26 minimal root\n' > "$R/etc/hakios-release"
chmod 644 "$R/etc/hakios-release"

# --- the image + verify with the kernel's own apfs_lib reader ---
python3 -I "$HK/tools/apfs/mkapfs.py" -z -o "$OUT/root-26.apfs" "System=$R" | tail -1
TOOL="$HK/apfs-work/apfs-tool"
"$TOOL" info "$OUT/root-26.apfs"
"$TOOL" stat "$OUT/root-26.apfs" /sbin/launchd
"$TOOL" cat  "$OUT/root-26.apfs" /sbin/launchd | cmp - "$OUT/launchd" \
	&& echo "root-26.apfs OK: /sbin/launchd round-trips byte-for-byte"
echo "root-26.apfs: $(stat -c %s "$OUT/root-26.apfs") bytes"
