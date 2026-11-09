#!/bin/sh
set -u
U=/home/mateusseiboth/Dev/mobile/research/UniXNU
WORK=$U/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work
FEED=$U/out/feeds/xnu-3199e8bc
DEV=/home/mateusseiboth/Dev/mobile/lisa-26-device
PATCHES=/home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/patches
cd "$U"
git config --global --add safe.directory '*' 2>/dev/null || true
echo "### reset pe_fiq.c"
cp "$FEED/pexpert/arm/pe_fiq.c" "$WORK/pexpert/arm/pe_fiq.c" 2>/dev/null || echo "  (no feed pe_fiq, skipping)"
echo "### re-apply darwin-lisa patches"
sh "$PATCHES/apply.sh" "$WORK"
echo "### build kernel"
python3 unixnu build kernel -D "$DEV"
echo "### BUILD EXIT=$?"
