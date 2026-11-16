#!/bin/sh
# darwin-lisa: build a kernel with the peripheral drivers (buttons, torch, haptic, and bt/audio when their wires exist)
# on the SHARED xnu-work tree, without touching kernel26.nopac and without re-applying kernel/patches (the tree is kept
# in a known state by its owners; apply.sh is theirs to run).
#
#   sh kernel/drivers/periph-build.sh NAME        -> research/hackios-xnu-kernel/NAME (e.g. kernel-16k-periph1.bak)
#
# - the whole run holds research/hackios-xnu-kernel/.build.lock (one build at a time on the shared tree);
# - the firmware header (proprietary blobs, never in git) is made on the host by periph-fw/periph-fw.py into a
#   temporary directory, and the haptic wire copies it into the tree;
# - every UniXNU step must succeed (no `|| true`), and before the copy the linked kernel must be newer than the start
#   of this run and define every symbol in EXPECT (nm): otherwise nothing is copied and the exit status is 1.
set -eu

NAME=${1:?usage: periph-build.sh KERNEL-NAME}
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa/kernel/drivers
LOCK=$M/research/hackios-xnu-kernel/.build.lock
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/periph-build.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT

python3 -I "$D/periph-fw/periph-fw.py" header "$STAGE/dlisa.periph_fw.h"

# the drivers this run wires, and the symbols the linked kernel must then define
WIRES="$D/buttons/buttons-wire.py $D/torch/torch-wire.py $D/haptic/haptic-wire.py"
EXPECT="_hakios_buttons_init _dlisa_torch_init _dlisa_haptic_init _dlisa_periph_file"
# EXTRA="bt audio" (the default) adds those drivers; EXTRA="" or EXTRA=audio leaves the others out
for extra in ${EXTRA-bt audio}; do
	case $extra in
	bt) WIRES="$WIRES $D/bt/bt-wire.py"; EXPECT="$EXPECT _dlisa_bt_init" ;;
	audio) WIRES="$WIRES $D/audio/audio-wire.py"; EXPECT="$EXPECT _hakios_sound_dsp_up _dlisa_tfa_look" ;;
	*) echo "periph-build: EXTRA knows bt and audio, not $extra" >&2; exit 2 ;;
	esac
done

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
for wire in $WIRES; do PERIPH_FW_HEADER=$STAGE/dlisa.periph_fw.h python3 "$wire"; done
if [ -f "$ATH11K" ]; then
	export XNU_EXTRA_LD_ARCHIVES="$ATH11K"
	[ "$ATH11K" -nt "$LINKED" ] && rm -f "$LINKED" "$LINKED.noctf"
fi
# the kernel step chains into prelink, which stops without the IPSW: its status is not the kernel's. What decides
# is the check below (the linked kernel new, the symbols there).
python3 unixnu build kernel -D "$DEV" || echo "periph-build: unixnu build kernel exited $? (checked below)"
[ "$LINKED" -nt "$STAGE/started" ] || { echo "periph-build: $LINKED is not newer than this run: the XNU link did not happen"; exit 1; }
python3 unixnu prelink --skip-prelink -D "$DEV"
[ "$KOUT/kernel.macho" -nt "$STAGE/started" ] || { echo "periph-build: kernel.macho is old"; exit 1; }
missing=""
for symbol in $EXPECT; do
	llvm-nm "$LINKED" | grep -q " [TtDdSs] $symbol\$" || missing="$missing $symbol"
done
[ -z "$missing" ] || { echo "periph-build: not in the linked kernel:$missing"; exit 1; }
echo "periph-build: linked kernel new, defines: $EXPECT"
python3 "$PACSTRIP" strip "$KOUT/kernel.macho" "$KOUT/kernel.macho.nopac"
python3 "$U/scripts/seal-source-crypto-kernel.py" "$KOUT/kernel.macho.nopac"
python3 "$PACSTRIP" scan "$KOUT/kernel.macho.nopac" | head -3
cp "$KOUT/kernel.macho.nopac" "$TARGET"
sha256sum "$TARGET"
echo "periph-build: DONE"
EOF

flock "$LOCK" docker run --rm -v $M:$M -v "$STAGE:$STAGE" -w $M/research/UniXNU -e HOME=/home/mateusseiboth \
	-e STAGE="$STAGE" -e WIRES="$WIRES" -e EXPECT="$EXPECT" -e TARGET="$M/research/hackios-xnu-kernel/$NAME" \
	darwin-lisa/unixnu-toolchain:latest sh "$STAGE/inside.sh"
