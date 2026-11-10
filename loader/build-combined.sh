#!/usr/bin/env bash
# darwin-lisa: the combined image of the one test session (docs/presentation-build.md, section 7).
#
#   darwin-lisa/loader/build-combined.sh
#   KERNEL=research/hackios-xnu-kernel/<final>.bak EXTRA_ARGS="rtc=1 battmgr=1" darwin-lisa/loader/build-combined.sh
#   -> out/vendor_boot-lisa-combined.img (+ loader-usb-lisa-combined.hkub, loader-combined.efi,
#      combined-payload.bin, combined-cmdline.txt)
#
# build-standalone.sh with: the iPhone 13 mini identity (D16AP, with its SoC and board: t8110, chip-id 0x8110, board-id 8,
# A2481, from identity-soc.py), the panel's own 1080x2400 (1:1 at 3x), the loader with run-tpl.patch, live-watch.patch
# and identity-soc.patch, and the standalone boot-args. Every variable of build-standalone.sh can
# still be set (AUTOBOOT, DATAVOL, FW_RAMDISK, PRESENT_EPOCH, ...). The identity header is Apple data: it stays out of
# git, in darwin-lisa/.work/identity (xnuloader/mkidentity.py makes it from DeviceTree.d16ap of the iPhone14,4 IPSW).
set -euo pipefail
M=$(cd "$(dirname "$0")/../.." && pwd)
export KERNEL=${KERNEL:-$M/research/hackios-xnu-kernel/kernel-16k-gpu1.bak}
export IDENTITY=${IDENTITY:-$M/darwin-lisa/.work/identity/identity-d16.h}
export GEOMETRY=${GEOMETRY-1080x2400}
export NAME=${NAME:-combined}
[[ -f $IDENTITY ]] || { echo "build-combined: no identity $IDENTITY (xnuloader/mkidentity.py)" >&2; exit 1; }
# the identity's own SoC and board (identity-soc.py), or the loader would still say t8015 / 0x8015 under an iPhone14,4
grep -q '^#define IDENTITY_CHIP_ID' "$IDENTITY" || { echo "build-combined: $IDENTITY has no SoC block:" \
	"darwin-lisa/loader/identity-soc.py ipsw/d16-dtree/DeviceTree.d16ap.bin $IDENTITY" >&2; exit 1; }
"$M/darwin-lisa/loader/build-standalone.sh"
line=$(cat "$M/out/$NAME-cmdline.txt")
echo "build-combined: the boot-arg line is ${#line} characters of at most 460 (room for $((460 - ${#line})) more)"
