#!/bin/sh
# darwin-lisa: a kernel for the UniXNU GPU stack's tests (T2), on the SHARED xnu-work tree, the way periph-build.sh builds:
#   - display-wire.py (copies kernel/drivers/display/: boot-arg fb_settle_us=N, the swap waits for a frame's submission that came
#     after it), then gpu-wire.py (copies kernel/drivers/gpu/hakios.gpu.cpp: the submit honours GPU_SUBMIT_BO_TRACK, and
#     MiniIOSurfaceSettled; re-applies its edits to the display's file) and kernel-wire.py here (boot-arg gpu_hwcg=1);
#   - the wires the tree already has (peripherals, display, ...) are left as they are: nothing is re-applied or reverted;
#   - the whole run holds research/hackios-xnu-kernel/.build.lock; kernel26.nopac is never written;
#   - before the copy the linked kernel must be newer than the start of the run, define every symbol in EXPECT and hold the
#     new code's strings; otherwise nothing is copied and the exit status is 1.
#
#   sh kernel/drivers/gpu/unixnu/build-kernel.sh NAME   -> research/hackios-xnu-kernel/NAME (e.g. kernel-16k-unixnu1.bak)
set -eu

NAME=${1:?usage: build-kernel.sh KERNEL-NAME}
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa/kernel/drivers
LOCK=$M/research/hackios-xnu-kernel/.build.lock
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/unixnu-kernel.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
case "$NAME" in kernel26.nopac|*/*) echo "build-kernel: not $NAME" >&2; exit 2 ;; esac

WIRES="$D/display/display-wire.py $D/gpu/gpu-wire.py $D/gpu/unixnu/kernel-wire.py"
EXPECT="_HakiosGPUStart _hakios_gpu_submit _hakios_gpu_parameter"
STRINGS="gpu_hwcg=1: Linux's clock gating;came after the swap; waited for"

cat > "$STAGE/inside.sh" <<'EOF'
set -eu
U=/home/mateusseiboth/Dev/mobile/research/UniXNU
DEV=/home/mateusseiboth/Dev/mobile/lisa-26-device
PACSTRIP=/home/mateusseiboth/Dev/mobile/darwin-lisa/tools/pacstrip/pacstrip.py
KOUT=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/kernel
OBJ=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837
LINKED=$OBJ/mach.development.bcm2837.unstripped
ATH11K=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/xnu-deps/build/ath11k/out/ath11k.o
cd "$U"
git config --global --add safe.directory '*' 2>/dev/null || true
touch "$STAGE/started"
for wire in $WIRES; do python3 "$wire"; done
if [ -f "$ATH11K" ]; then
	export XNU_EXTRA_LD_ARCHIVES="$ATH11K"
	[ "$ATH11K" -nt "$LINKED" ] && rm -f "$LINKED" "$LINKED.noctf"
fi
# the kernel step chains into prelink, which stops without the IPSW: its status is not the kernel's. What decides
# is the check below (the linked kernel new, the symbols there).
python3 unixnu build kernel -D "$DEV" || echo "build-kernel: unixnu build kernel exited $? (checked below)"
[ "$LINKED" -nt "$STAGE/started" ] || { echo "build-kernel: $LINKED is not newer than this run: the XNU link did not happen"; exit 1; }
python3 unixnu prelink --skip-prelink -D "$DEV"
[ "$KOUT/kernel.macho" -nt "$STAGE/started" ] || { echo "build-kernel: kernel.macho is old"; exit 1; }
missing=""
for symbol in $EXPECT; do
	llvm-nm "$LINKED" | grep -q " [TtDdSs] $symbol\$" || missing="$missing $symbol"
done
[ -z "$missing" ] || { echo "build-kernel: not in the linked kernel:$missing"; exit 1; }
strings "$LINKED" > "$STAGE/strings"
echo "$STRINGS" | tr ';' '\n' | while read -r text; do
	grep -qF "$text" "$STAGE/strings" || { echo "build-kernel: the linked kernel lacks the new code ($text)"; exit 1; }
done
echo "build-kernel: linked kernel new, defines: $EXPECT"
python3 "$PACSTRIP" strip "$KOUT/kernel.macho" "$KOUT/kernel.macho.nopac"
python3 "$U/scripts/seal-source-crypto-kernel.py" "$KOUT/kernel.macho.nopac"
python3 "$PACSTRIP" scan "$KOUT/kernel.macho.nopac" | head -3
cp "$KOUT/kernel.macho.nopac" "$TARGET"
sha256sum "$TARGET"
echo "build-kernel: DONE"
EOF

flock "$LOCK" docker run --rm -v $M:$M -v "$STAGE:$STAGE" -w $M/research/UniXNU -e HOME=/home/mateusseiboth \
	-e STAGE="$STAGE" -e WIRES="$WIRES" -e EXPECT="$EXPECT" -e STRINGS="$STRINGS" -e TARGET="$M/research/hackios-xnu-kernel/$NAME" \
	darwin-lisa/unixnu-toolchain:latest sh "$STAGE/inside.sh"
