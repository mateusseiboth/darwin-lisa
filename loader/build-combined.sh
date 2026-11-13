#!/usr/bin/env bash
# darwin-lisa: the combined image of the one test session (docs/presentation-build.md, section 7).
#
#   darwin-lisa/loader/build-combined.sh                         (the evening build: kernel-16k-final1.bak)
#   KERNEL=research/hackios-xnu-kernel/<other>.bak EXTRA_ARGS="..." darwin-lisa/loader/build-combined.sh
#   -> out/vendor_boot-lisa-combined.img (+ loader-usb-lisa-combined.hkub, loader-combined.efi,
#      combined-payload.bin, combined-cmdline.txt)
#
# build-standalone.sh with: the iPhone 13 mini identity (D16AP, with its SoC and board: t8110, chip-id 0x8110, board-id 8,
# A2481, from identity-soc.py), the panel's own 1080x2400 (1:1 at 3x), the loader with run-tpl.patch, live-watch.patch
# and identity-soc.patch, and the standalone boot-args. Every variable of build-standalone.sh can
# still be set (AUTOBOOT, DATAVOL, FW_RAMDISK, PRESENT_EPOCH, ...). The identity header is Apple data: it stays out of
# git, in darwin-lisa/.work/identity (xnuloader/mkidentity.py makes it from DeviceTree.d16ap of the iPhone14,4 IPSW).
#
# The evening build (docs/evening-session.md) is the default now: kernel-16k-final1.bak, lisa=1 (the kernel's built-in
# hardware arguments: LISA_DEFAULTS=1), the Wi-Fi radio up (WIFI=1: it joins the network kept on the data volume),
# and FEATURES="rtc=1 adsp=1 battmgr=2" (the PMIC clock, the audio DSP with the fuel gauge and charging). The GPU and
# the sensors stay off: they are tested in host runs. FEATURES= (empty), WIFI=0 or LISA_DEFAULTS=0 take them out;
# EXTRA_ARGS comes after FEATURES.
set -euo pipefail
M=$(cd "$(dirname "$0")/../.." && pwd)
export KERNEL=${KERNEL:-$M/research/hackios-xnu-kernel/kernel-16k-final1.bak}
export IDENTITY=${IDENTITY:-$M/darwin-lisa/.work/identity/identity-d16.h}
export GEOMETRY=${GEOMETRY-1080x2400}
export NAME=${NAME:-combined}
export LISA_DEFAULTS=${LISA_DEFAULTS:-1}
export WIFI=${WIFI:-1}
FEATURES=${FEATURES-rtc=1 adsp=1 battmgr=2}
export EXTRA_ARGS="$FEATURES${EXTRA_ARGS:+ $EXTRA_ARGS}"
[[ -f $IDENTITY ]] || { echo "build-combined: no identity $IDENTITY (xnuloader/mkidentity.py)" >&2; exit 1; }
# the identity's own SoC and board (identity-soc.py), or the loader would still say t8015 / 0x8015 under an iPhone14,4
grep -q '^#define IDENTITY_CHIP_ID' "$IDENTITY" || { echo "build-combined: $IDENTITY has no SoC block:" \
	"darwin-lisa/loader/identity-soc.py ipsw/d16-dtree/DeviceTree.d16ap.bin $IDENTITY" >&2; exit 1; }
# and its /buttons "home-button-type" (2 on the 13 mini), or MobileGestalt says "home button" (docs/home-button-type.md)
grep -q '^#define IDENTITY_HOME_BUTTON_TYPE' "$IDENTITY" || { echo "build-combined: $IDENTITY has no home-button-type:" \
	"darwin-lisa/loader/identity-soc.py ipsw/d16-dtree/DeviceTree.d16ap.bin $IDENTITY" >&2; exit 1; }
"$M/darwin-lisa/loader/build-standalone.sh"
line=$(cat "$M/out/$NAME-cmdline.txt")
echo "build-combined: the boot-arg line is ${#line} characters of at most 460 (room for $((460 - ${#line})) more)"
