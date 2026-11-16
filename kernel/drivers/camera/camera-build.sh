#!/bin/sh
# darwin-lisa: the camera wired into the shared xnu-work and the kernel rebuilt, into a kernel of its own.
#
#   sh darwin-lisa/kernel/drivers/camera/camera-build.sh [NAME]     (default NAME kernel-16k-cam1)
#   -> research/hackios-xnu-kernel/NAME.bak, its sha256 printed; logs/NAME-build.log
#
# Holds research/hackios-xnu-kernel/.build.lock (flock) from the wire until the .bak is copied: the kernel workers
# take turns at the one tree. Nothing of the tree is reverted, nothing of the others' wires touched; apply.sh is not run
# (the tree already carries it), and kernel26.nopac is left alone. The build is .work/rebuild26.sh's steps 3-6.
set -eu
NAME=${1:-kernel-16k-cam1}
M=/home/mateusseiboth/Dev/mobile
K=$M/research/hackios-xnu-kernel
U=$M/research/UniXNU
LOG=$M/darwin-lisa/logs/$NAME-build.log
mkdir -p "$(dirname "$LOG")"
exec flock "$K/.build.lock" docker run --rm -v "$M:$M" -w "$U" -e HOME=/home/mateusseiboth -e NAME="$NAME" \
	darwin-lisa/unixnu-toolchain:latest sh -c '
set -eu
M=/home/mateusseiboth/Dev/mobile
U=$M/research/UniXNU
DEV=$M/lisa-26-device
KOUT=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/kernel
PACSTRIP=$M/darwin-lisa/tools/pacstrip/pacstrip.py
git config --global --add safe.directory "*" 2>/dev/null || true
echo "### 1. camera wire"
python3 $M/darwin-lisa/kernel/drivers/camera/camera-wire.py
echo "### 2. build kernel"
ATH11K=$M/research/hackios-xnu-kernel/xnu-deps/build/ath11k/out/ath11k.o
LINKED=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped
if [ -f "$ATH11K" ]; then
  export XNU_EXTRA_LD_ARCHIVES="$ATH11K"
  [ "$ATH11K" -nt "$LINKED" ] && rm -f "$LINKED" "$LINKED.noctf"
fi
python3 unixnu build kernel -D "$DEV" || true
python3 unixnu prelink --skip-prelink -D "$DEV"
grep -aq "camera: cam=%u: the %s camera" "$KOUT/kernel.macho" || { echo "### the camera driver is NOT in the kernel"; exit 1; }
echo "### 3. pacstrip + reseal"
python3 "$PACSTRIP" strip "$KOUT/kernel.macho" "$KOUT/kernel.macho.nopac"
python3 "$U/scripts/seal-source-crypto-kernel.py" "$KOUT/kernel.macho.nopac"
python3 "$PACSTRIP" scan "$KOUT/kernel.macho.nopac" | head -3
cp "$KOUT/kernel.macho.nopac" "$M/research/hackios-xnu-kernel/$NAME.bak"
chown 1000:1000 "$M/research/hackios-xnu-kernel/$NAME.bak" 2>/dev/null || true
sha256sum "$M/research/hackios-xnu-kernel/$NAME.bak"
echo "### CAMERA BUILD DONE"
' >"$LOG" 2>&1
