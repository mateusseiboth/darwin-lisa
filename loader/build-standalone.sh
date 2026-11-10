#!/usr/bin/env bash
# darwin-lisa: the "presentation build": a vendor_boot whose loader boots iOS 26 by itself, with no PC.
#
#   darwin-lisa/loader/build-standalone.sh            (environment below)
#   -> out/vendor_boot-lisa-$NAME.img                 for vendor_boot_a (a WRITE: docs/presentation-build.md)
#   -> out/loader-usb-lisa-$NAME.hkub                 the same HKUB alone, for U-Boot's "HakiOS over fastboot" (RAM)
#   -> out/loader-$NAME.efi                           the loader
#   -> out/$NAME-payload.bin, out/$NAME-cmdline.txt   (NAME=standalone by default)
#
# The chain on a cold power-on is unchanged up to the loader: ABL, U-Boot's menu (5 s, volume keys pick pmOS),
# "hakios load" takes this HKUB from vendor_boot's ramdisk and puts the payload at 0xf0000000. The payload now carries
# the kernel and the boot-args, with "usb" and "autoboot=N": the loader waits N s for a host (any fastboot command
# keeps it, as before) and otherwise runs the kernel itself, exactly as "fastboot oem run" would
# (loader/autoboot.patch, on top of oem-bootos.patch and data-region.patch). run-tpl.patch keeps U-Boot's console
# from polling the keys over the PMIC arbiter during a run; live-watch.patch reads the kernel's signs of life through
# memory and never stops serving its storage while it holds its core (docs/presentation-build.md, section 7).
# identity-soc.patch writes the identity's own platform-name, chip-id, board-id and regulatory model when the identity
# header carries them (loader/identity-soc.py), the iPhone X's otherwise.
#
# Sources are copied to darwin-lisa/.work/loader-standalone/build and patched there: research/hackios-xnu-kernel/xnuloader
# is never touched. Nothing here writes to the phone.
#
# Environment:
#   KERNEL        the kernel (default research/hackios-xnu-kernel/kernel26.nopac). Use the presentation kernel
#   AUTOBOOT      seconds the loader waits for a host (default 10)
#   DATAVOL       1 (default): the persistent /private/var in super's tail (ufs_write=1 ufs_data=super var_ufs=1);
#                 0: ramfs /private/var (every boot is a first boot: much slower)
#   FW_RAMDISK    an APFS image carried as the resident md0 (the WiFi firmware: userland-26/out/fw-26-pad32.apfs).
#                 Empty (default): no WiFi (wlan_stub=1)
#   PRESENT_EPOCH the wall clock the kernel starts from (no RTC is read anywhere): default the build time
#   IDENTITY      the identity header the loader is built with (default research/hackios-xnu-kernel/xnuloader/identity.h,
#                 the iPhone X, D22). darwin-lisa/.work/identity/identity-d16.h is the iPhone 13 mini (D16AP), made by
#                 xnuloader/mkidentity.py from DeviceTree.d16ap of the iPhone14,4 23F84 IPSW (Apple data: not in git)
#   GEOMETRY      WxH: the display iOS is told about (fbwidth= fbheight=, kernel/drivers/display). 1080x2400 is the
#                 panel itself, 1:1 at 3x (360 x 800 points); 1125x2499 is 375 x 833 points shown at 0.96. Empty (default):
#                 nothing added, the kernel's 1125x2436 (the iPhone X's, squeezed onto the panel)
#   NAME          the name in the output files (default standalone). Never the name of an image already flashed
#   LISA_DEFAULTS 1: lisa=1 instead of the hardware arguments (touchscreen=1 touch_avdd_ldo= touch_chip= touch_goodix_a=
#                 cpufreq=1 cpufreq_max= and, with WIFI=1, wifi_irq= wifi_mac=): the kernel has them built in
#                 (kernel/drivers/bootargs/lisa-defaults-wire.py), and the line keeps room. Refused with a kernel
#                 without them. 0 (default): the arguments themselves, as before
#   WIFI          1: wifi=1 (the radio up; it joins the network kept on the data volume, and init26 lets wifid run)
#                 instead of wlan_stub=1. 0 (default): wlan_stub=1
#   EXTRA_ARGS    more boot-args, appended
#   ARGS          the whole boot-arg line, replacing the default (EXTRA_ARGS still appended)
set -euo pipefail
M=$(cd "$(dirname "$0")/../.." && pwd)
H=$M/research/hackios-xnu-kernel
W=$M/darwin-lisa/.work/loader-standalone
KERNEL=${KERNEL:-$H/kernel26.nopac}
IDENTITY=${IDENTITY:-$H/xnuloader/identity.h}
AUTOBOOT=${AUTOBOOT:-10}
DATAVOL=${DATAVOL:-1}
FW_RAMDISK=${FW_RAMDISK:-}
GEOMETRY=${GEOMETRY:-}
NAME=${NAME:-standalone}
LISA_DEFAULTS=${LISA_DEFAULTS:-0}
WIFI=${WIFI:-0}
EPOCH=${PRESENT_EPOCH:-$(date +%s)}
BASE_VB=$H/out/vendor_boot-multiboot-lisa.img        # header fields and the DTB (the same bytes pmOS gets)
PART_BYTES=100663296                                  # vendor_boot_a: 24576 blocks of 4096
PAYLOAD_MAX=$((0x4000000 - 0x100000))                 # the loader's payload region less the panic log

[[ -f $KERNEL ]] || { echo "build-standalone: no kernel $KERNEL" >&2; exit 1; }
[[ -f $BASE_VB ]] || { echo "build-standalone: no $BASE_VB" >&2; exit 1; }
[[ $NAME =~ ^[a-z0-9][a-z0-9-]*$ ]] || { echo "build-standalone: NAME=$NAME (lower case, digits, -)" >&2; exit 1; }
VB_OUT=$M/out/vendor_boot-lisa-$NAME.img
HKUB_OUT=$M/out/loader-usb-lisa-$NAME.hkub
EFI_OUT=$M/out/loader-$NAME.efi
PAYLOAD_OUT=$M/out/$NAME-payload.bin
CMDLINE_OUT=$M/out/$NAME-cmdline.txt
FB=""
if [[ -n $GEOMETRY ]]; then
	[[ $GEOMETRY =~ ^([0-9]{3,4})x([0-9]{3,4})$ ]] || { echo "build-standalone: GEOMETRY=$GEOMETRY (WxH)" >&2; exit 1; }
	FB="fbwidth=${BASH_REMATCH[1]} fbheight=${BASH_REMATCH[2]}"
fi

# --- the boot-args -----------------------------------------------------------------------------------------------
# Base: the flash root with the loader on core 0 (bootcpu=7: it feeds the hypervisor watchdog and serves the UFS
# mailbox). loader_return_after=86400: the loader feeds the watchdog for the whole day too (budget + 60 s), and the
# kernel's own hv_wdt_pet timer does as well. No -v (text over the UI), no touch_synth (a real hand).
# dlisa_*: libdlisa in SpringBoard (userland/inject/presentation-springboard.patch, inside the root image): unlock once
# at boot, no auto-lock, FrontBoard's launch watchdog x4. A root without that patch ignores them.
ROOT="rd=md0 ufs=2 ufs_part=super ufs_root=1"
[[ $DATAVOL = 1 ]] && ROOT="$ROOT ufs_write=1 ufs_data=super var_ufs=1"
WLAN="wlan_stub=1"
[[ -n $FW_RAMDISK ]] && WLAN="wifi=1 wifi_mac=02:6c:69:73:61:01"
[[ $WIFI = 1 ]] && WLAN="wifi=1 wifi_irq=0 wifi_mac=02:6c:69:73:61:01"
HW="touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix touch_goodix_a=1 cpus=7 cpufreq=1 cpufreq_max=2400"
# lisa=1: the kernel's built-in hardware arguments (the same values); cpus=7 stays, the loader reads it
if [[ $LISA_DEFAULTS = 1 ]]; then
	grep -aq 'touch_avdd_ldo=ldoc7 touch_chip=goodix' "$KERNEL" ||
		{ echo "build-standalone: LISA_DEFAULTS=1, but $KERNEL has no lisa=1 defaults (lisa-defaults-wire.py)" >&2; exit 1; }
	HW="lisa=1 cpus=7"
	WLAN=${WLAN/ wifi_irq=0/}
	WLAN=${WLAN/ wifi_mac=02:6c:69:73:61:01/}
fi
# (the loader puts debug=0x8 rd=md0 hv_wdt_pet=1 and its own *_pa= / hakios_kver= in front and cuts the whole at 607
# characters: what the kernel must see comes first, and the check below keeps the line short. sb_delay=8 is gone: only
# HakiOS 16.5's libhakios.sb read it, and nothing in the iOS 26 kernel or root does)
DEFAULT_ARGS="$ROOT dlisa_unlock=1 dlisa_noidle=1 dlisa_watchdog=4 display=1 keystore=1 console_open=1 \
$HW battery=1 $WLAN init_run=/usr/libexec/usermanagerd:--init init_run_secs=60 bootcpu=7 loader_return_after=86400 \
epoch=$EPOCH"
LINE="${ARGS:-$DEFAULT_ARGS}${FB:+ $FB}${EXTRA_ARGS:+ $EXTRA_ARGS}"
LINE=$(tr -s ' ' <<<"$LINE")
case " $LINE " in *" fbwidth="*" fbwidth="*|*" fbheight="*" fbheight="*)
	echo "build-standalone: fbwidth/fbheight twice (GEOMETRY and EXTRA_ARGS)" >&2; exit 1 ;; esac
case " $LINE " in *" touch_synth="*) echo "build-standalone: touch_synth in the line (the demo is by hand)" >&2; exit 1 ;; esac
# the loader's own words plus this must stay under its 607. With ufs=2 and battery=1 they are 118 characters
# (debug=0x8 hv_wdt_pet=1 ufs_shared_pa=0x... battery_shared_pa=0x... hakios_kver=dl26-xxxxxxxx: "cmdline: 526 of 607" with
# a 408-character line, 2026-11-10); 460 leaves room for a few more of them
[ ${#LINE} -le 460 ] || { echo "build-standalone: the line is ${#LINE} characters (at most 460): the kernel would lose its end" >&2; exit 1; }
case " $LINE " in *" bootcpu=7 "*) ;; *) echo "build-standalone: bootcpu=7 is required (watchdog + UFS mailbox)" >&2; exit 1 ;; esac

# --- the loader --------------------------------------------------------------------------------------------------
rm -rf "${W:?}/build" && mkdir -p "$W/build/xnuloader"
cp "$H"/xnuloader/{loader.c,efi.h,build.sh} "$W/build/xnuloader/"
cp "$IDENTITY" "$W/build/xnuloader/identity.h"
SOC=$(sed -n 's/^#define IDENTITY_\(PLATFORM_NAME\|CHIP_ID\|BOARD_ID\|REGULATORY_MODEL\) *\(.*\)$/\2/p' "$IDENTITY" | tr -d '"' | tr '\n' ' ')
[[ -n $SOC ]] || SOC="none in the header, so the defaults: t8015 0x8015 0x06 A1865"
echo "build-standalone: identity SoC and board: $SOC"
cp "$M/darwin-lisa/loader/data-regions.h" "$W/build/xnuloader/"
# boot-args-layout.patch (xnu-12377's 1024-byte boot_args command line, the whole boot_args page zeroed) is opt-in until
# a session at the phone has booted it: LOADER_BOOTARGS_FIX=1. The kernel copes with the old layout either way
# (kernel/patches/boot-args-loader-layout.patch).
LOADER_PATCHES="oem-bootos.patch data-region.patch autoboot.patch run-tpl.patch live-watch.patch identity-soc.patch"
[ "${LOADER_BOOTARGS_FIX:-0}" = 1 ] && LOADER_PATCHES="$LOADER_PATCHES boot-args-layout.patch"
for p in $LOADER_PATCHES; do
	(cd "$W/build" && patch -s -p1 --forward <"$M/darwin-lisa/loader/$p")
done
(cd "$W/build/xnuloader" && bash build.sh >/dev/null)
grep -aq 'autoboot: iOS starts in' "$W/build/xnuloader/loader.efi" || { echo "build-standalone: loader lacks autoboot" >&2; exit 1; }
grep -aq 'still watching it and serving its storage' "$W/build/xnuloader/loader.efi" ||
	{ echo "build-standalone: loader lacks live-watch" >&2; exit 1; }
PLATFORM_NAME=$(sed -n 's/^#define IDENTITY_PLATFORM_NAME *"\(.*\)"$/\1/p' "$IDENTITY")
grep -aq "${PLATFORM_NAME:-t8015}" "$W/build/xnuloader/loader.efi" ||
	{ echo "build-standalone: loader lacks the platform name ${PLATFORM_NAME:-t8015}" >&2; exit 1; }

# --- the payload and the HKUB ------------------------------------------------------------------------------------
KVER="dl26-$(sha256sum "$KERNEL" | cut -c1-8)"      # (short: it is in the 607 characters too)
items=(kernel="$KERNEL")
[[ -n $FW_RAMDISK ]] && items+=(resident="$FW_RAMDISK")
printf '%s\n' "$LINE" >"$CMDLINE_OUT"
# (mkpayload has no --autoboot: the entry is added by hand in the same layout, before the end marker)
python3 -I "$H/xnuloader/mkpayload.py" -o "$W/payload-noauto.bin" "${items[@]}" --cmdline "$LINE" --usb \
	--kernel-version "$KVER" >/dev/null
python3 -I - "$W/payload-noauto.bin" "$PAYLOAD_OUT" "$AUTOBOOT" "$PAYLOAD_MAX" <<'EOF'
import struct, sys
src, out, secs, most = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
d = open(src, 'rb').read()
assert d[:4] == b'XNUP'
body = d[8:]
# walk to the end marker (an entry with an empty name)
p = 0
while body[p] != 0:
    size = struct.unpack_from('<I', body, p + 16)[0]
    p += 24 + ((size + 15) & ~15)
text = secs.encode() + b'\0'
entry = struct.pack('<16sII', b'autoboot', len(text), 0) + text + b'\0' * (-len(text) % 16)
body = body[:p] + entry + body[p:]
img = struct.pack('<II', 0x50554E58, len(body)) + body
assert len(img) <= most, f'payload {len(img)} > {most}'
open(out, 'wb').write(img)
print(f'{out}: {len(img)} bytes ({len(img) / 1048576:.1f} MiB of {most / 1048576:.0f}), autoboot={secs}')
EOF
python3 -I "$H/tools/mkubootimg.py" -d lisa -l "$W/build/xnuloader/loader.efi" -p "$PAYLOAD_OUT" \
	-c "$HKUB_OUT"

# --- vendor_boot: the base image's header and DTB, this HKUB as the vendor ramdisk ---------------------------------
python3 -I - "$BASE_VB" "$HKUB_OUT" "$VB_OUT" "$PART_BYTES" <<'EOF'
import struct, sys
src, hkub, out, part = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
d = open(src, 'rb').read()
assert d[:8] == b'VNDRBOOT' and struct.unpack_from('<I', d, 8)[0] == 3
page, vr = struct.unpack_from('<I', d, 12)[0], struct.unpack_from('<I', d, 24)[0]
hs, dtb_size = struct.unpack_from('<II', d, 2096)
up = lambda n: (n + page - 1) // page * page
at = up(hs)
dtb_at = at + up(vr)
dtb = d[dtb_at:dtb_at + dtb_size]
assert len(d) == dtb_at + up(dtb_size), 'unexpected layout of the base vendor_boot'
new = open(hkub, 'rb').read()
hdr = bytearray(d[:at])
struct.pack_into('<I', hdr, 24, len(new))
pad = lambda b: b + bytes(-len(b) % page)
img = bytes(hdr) + pad(new) + pad(dtb)
# ABL (stock abl.elf, LinuxLoader): an arm64 kernel gets 0x5600000 bytes for kernel + 2 MiB DTB + all ramdisks
assert len(new) + 2 * 1048576 + 2 * 1048576 < 0x5600000, 'ramdisk too large for ABL'
assert len(img) <= part, f'{len(img)} > vendor_boot_a {part}'
open(out, 'wb').write(img)
print(f'{out}: {len(img)} bytes: vendor ramdisk {vr} -> {len(new)}, DTB {dtb_size} bytes (same) at {at + len(pad(new)):#x}')
EOF
cp "$W/build/xnuloader/loader.efi" "$EFI_OUT"
echo "boot-args: $LINE"
sha256sum "$KERNEL" "$PAYLOAD_OUT" "$HKUB_OUT" \
	"$VB_OUT" "$EFI_OUT"
