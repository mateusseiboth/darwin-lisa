#!/bin/sh
# darwin-lisa: rebuild the 26 kernel with the current darwin-lisa patches, then
# pacstrip + reseal, and drop kernel26.nopac next to the loader. Runs inside the
# docker toolchain (needs /opt/unixnu-linux). Mirrors the established flow.
set -eu

U=/home/mateusseiboth/Dev/mobile/research/UniXNU
WORK=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work
FEED=$U/out/feeds/xnu-3199e8bc
DEV=/home/mateusseiboth/Dev/mobile/lisa-26-device
PATCHES=/home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/patches
PACSTRIP=/home/mateusseiboth/Dev/mobile/darwin-lisa/tools/pacstrip/pacstrip.py
KOUT=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/kernel
TARGET=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/kernel26.nopac

cd "$U"
git config --global --add safe.directory '*' 2>/dev/null || true

echo "### 1. reset pe_fiq.c to pristine so the updated patch applies clean"
cp "$FEED/pexpert/arm/pe_fiq.c" "$WORK/pexpert/arm/pe_fiq.c"

echo "### 2. re-apply darwin-lisa patches (copies gic_vtimer.c + applies pe_fiq patch)"
sh "$PATCHES/apply.sh" "$WORK"

echo "### 3. build kernel"
# The Wi-Fi driver ath11k (kernel/drivers/wifi-ath11k-wire.sh -> HakiOS tools/mk-ath11k.sh), linked when it is there.
# The UniXNU build sets LD_KERNEL_ARCHIVES on make's command line; XNU_EXTRA_LD_ARCHIVES is its hook for more.
# make does not see the object change, so the linked kernel is dropped when ath11k.o is newer than it.
ATH11K=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/xnu-deps/build/ath11k/out/ath11k.o
LINKED=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped
if [ -f "$ATH11K" ]; then
  export XNU_EXTRA_LD_ARCHIVES="$ATH11K"
  [ "$ATH11K" -nt "$LINKED" ] && rm -f "$LINKED" "$LINKED.noctf"
  echo "  ath11k.o linked in: $ATH11K"
fi
python3 unixnu build kernel -D "$DEV" || true   # build chains into prelink which stops w/o IPSW

echo "### 4. stage native (--skip-prelink) -> kernel.macho"
python3 unixnu prelink --skip-prelink -D "$DEV"

echo "### 5. verify my marker compiled in (before stripping)"
if grep -aq "vtimer G1 armed" "$KOUT/kernel.macho"; then
  echo "  OK: pe_fiq Group-1 marker present in linked kernel"
else
  echo "  WARNING: marker NOT found in kernel.macho (patch may not have compiled)"
fi

echo "### 6. pacstrip + reseal"
python3 "$PACSTRIP" strip "$KOUT/kernel.macho" "$KOUT/kernel.macho.nopac"
python3 "$U/scripts/seal-source-crypto-kernel.py" "$KOUT/kernel.macho.nopac"
echo "  pacstrip scan (should be 0 PAC):"
python3 "$PACSTRIP" scan "$KOUT/kernel.macho.nopac" | head -3

echo "### 7. copy to loader"
cp "$KOUT/kernel.macho.nopac" "$TARGET"
ls -la "$TARGET"
echo "### REBUILD DONE"
