#!/bin/sh
# darwin-lisa: a kernel with this tree's speakers driver (audio-wire.py: dlisa.sound.c, dlisa.tfa9873.c and its two
# headers) on the SHARED xnu-work tree, the way periph-build.sh and the GPU's build-kernel.sh build:
#   - only the audio wire runs; every other wire the tree has (peripherals, display, GPU, brightness, ...) is left as it
#     is: nothing is re-applied or reverted, and kernel26.nopac is never written;
#   - the whole run holds research/hackios-xnu-kernel/.build.lock;
#   - before the copy the linked kernel must be newer than the start of the run, define the audio's symbols and the GPU
#     line's (unixnu4: gpu_irq), and hold the new code's strings; otherwise nothing is copied and the exit status is 1.
#
#   sh kernel/drivers/audio/audio-build.sh NAME   -> research/hackios-xnu-kernel/NAME (e.g. kernel-16k-audio1.bak)
set -eu

NAME=${1:?usage: audio-build.sh KERNEL-NAME}
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa/kernel/drivers
LOCK=$M/research/hackios-xnu-kernel/.build.lock
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/audio-build.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
case "$NAME" in kernel26.nopac|*/*) echo "audio-build: not $NAME" >&2; exit 2 ;; esac
sh "$D/audio/test/run-tests.sh"

WIRES="$D/audio/audio-wire.py"
EXPECT="_hakios_sound_dsp_up _dlisa_tfa_look _dlisa_tfa_configure _HakiosGPUStart _hakios_gpu_irq _gic_spi_handler_set"
STRINGS="Xiaomi's tuning;keys 1 and 2 opened;die temperature over the driver's limit;sound_tune %u"

cat > "$STAGE/inside.sh" <<'INSIDE'
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
# the kernel step chains into prelink, which stops without the IPSW: its status is not the kernel's (checked below)
python3 unixnu build kernel -D "$DEV" || echo "audio-build: unixnu build kernel exited $? (checked below)"
[ "$LINKED" -nt "$STAGE/started" ] || { echo "audio-build: $LINKED is not newer than this run: the XNU link did not happen"; exit 1; }
python3 unixnu prelink --skip-prelink -D "$DEV"
[ "$KOUT/kernel.macho" -nt "$STAGE/started" ] || { echo "audio-build: kernel.macho is old"; exit 1; }
missing=""
for symbol in $EXPECT; do
	llvm-nm "$LINKED" | grep -q " [TtDdSs] $symbol\$" || missing="$missing $symbol"
done
[ -z "$missing" ] || { echo "audio-build: not in the linked kernel:$missing"; exit 1; }
strings "$LINKED" > "$STAGE/strings"
OLDIFS=$IFS; IFS=';'
for text in $STRINGS; do
	grep -qF "$text" "$STAGE/strings" || { echo "audio-build: the linked kernel lacks the new code ($text)"; exit 1; }
done
IFS=$OLDIFS
echo "audio-build: linked kernel new, defines: $EXPECT"
python3 "$PACSTRIP" strip "$KOUT/kernel.macho" "$KOUT/kernel.macho.nopac"
python3 "$U/scripts/seal-source-crypto-kernel.py" "$KOUT/kernel.macho.nopac"
python3 "$PACSTRIP" scan "$KOUT/kernel.macho.nopac" | head -3
cp "$KOUT/kernel.macho.nopac" "$TARGET"
sha256sum "$TARGET"
echo "audio-build: DONE"
INSIDE

flock "$LOCK" docker run --rm -v $M:$M -v "$STAGE:$STAGE" -w $M/research/UniXNU -e HOME=/home/mateusseiboth \
	-e STAGE="$STAGE" -e WIRES="$WIRES" -e EXPECT="$EXPECT" -e STRINGS="$STRINGS" \
	-e TARGET="$M/research/hackios-xnu-kernel/$NAME" darwin-lisa/unixnu-toolchain:latest sh "$STAGE/inside.sh"
