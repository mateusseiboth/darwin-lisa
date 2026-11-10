#!/bin/bash
# darwin-lisa: put the last known-good standalone boot back on the phone (no GPU UI, software rendering).
#
#   PMOS_PASS=... darwin-lisa/tools/restore-safe.sh
#
# Known-good set (2026-11-10, standalone cold boot reached the home screen at the right local time):
#   vendor_boot_a = out/vendor_boot-lisa-safe-66c997f7.img  (fixed loader boot_args layout + kernel-16k-final1-fix,
#                   autoboot line: lisa=1 rtc=1 adsp=1 battmgr=2 wifi=1 noidle=1 tz=America/Campo_Grande ...)
#   super root    = userland-26/out/system-26.apfs.full-6db16457 (already on the phone; rewritten only with ROOT=1)
# The data region (super's tail, the iOS /private/var) is never touched.
#
# What it does: brings the phone to pmOS (bootos.sh pmos, held awake), copies the image, writes vendor_boot_a,
# reads it back and compares sha256, optionally does the same for the root region, then reboots into the loader,
# which autoboots iOS after 10 s when no host talks to it.
set -euo pipefail
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa
VB=$M/out/vendor_boot-lisa-safe-66c997f7.img
VB_SHA=66c997f72694f924f181646f78ac504b6b52aafb0e575771020c53203bc1a0ae
ROOT_IMG=$M/userland-26/out/system-26.apfs.full-6db16457
ROOT_SHA=6db16457ef9855ef237901285de972e04782347ee9b22eb53b7f6d606158adc6
ROOT_BYTES=4831838208

[[ -n ${PMOS_PASS:-} ]] || { echo "set PMOS_PASS (the pmOS password; it is never written anywhere)" >&2; exit 1; }
command -v sshpass >/dev/null || { echo "sshpass is needed" >&2; exit 1; }
say() { echo "restore-safe: $*"; }
export SSHPASS=$PMOS_PASS
SSH=(sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR mateus@172.16.42.1)
SCP=(sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
root_sh() { printf '%s\n' "$PMOS_PASS" | "${SSH[@]}" "sudo -S -p '' sh -c '$1'"; }

[[ $(sha256sum "$VB" | cut -d' ' -f1) == "$VB_SHA" ]] || { echo "$VB is not $VB_SHA" >&2; exit 1; }

say "to pmOS"
bash "$D/tools/bootos.sh" pmos

size=$(stat -c %s "$VB")
say "copying vendor_boot ($size bytes)"
"${SCP[@]}" "$VB" mateus@172.16.42.1:/home/mateus/vendor_boot-safe.img
say "writing vendor_boot_a"
root_sh "dd if=/home/mateus/vendor_boot-safe.img of=/dev/disk/by-partlabel/vendor_boot_a bs=1M conv=fsync 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches"
got=$(root_sh "head -c $size /dev/disk/by-partlabel/vendor_boot_a | sha256sum" | cut -d' ' -f1)
[[ $got == "$VB_SHA" ]] || { echo "vendor_boot_a read-back is $got, not $VB_SHA: NOT rebooting, phone stays in pmOS" >&2; exit 1; }
say "vendor_boot_a verified ($VB_SHA)"

if [[ ${ROOT:-0} == 1 ]]; then
	[[ $(sha256sum "$ROOT_IMG" | cut -d' ' -f1) == "$ROOT_SHA" ]] || { echo "$ROOT_IMG is not $ROOT_SHA" >&2; exit 1; }
	say "copying the root image (4.8 GB)"
	"${SCP[@]}" "$ROOT_IMG" mateus@172.16.42.1:/home/mateus/system-26-safe.apfs
	say "writing the root region of super (the data region after it is not touched)"
	root_sh "dd if=/home/mateus/system-26-safe.apfs of=/dev/disk/by-partlabel/super bs=1M count=4608 conv=notrunc,fsync 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches"
	got=$(root_sh "head -c $ROOT_BYTES /dev/disk/by-partlabel/super | sha256sum" | cut -d' ' -f1)
	[[ $got == "$ROOT_SHA" ]] || { echo "root read-back is $got, not $ROOT_SHA: NOT rebooting" >&2; exit 1; }
	say "root verified ($ROOT_SHA)"
fi

say "rebooting into the loader; it autoboots iOS in 10 s if nothing talks to it over USB"
root_sh "systemctl reboot" || true
say "done. Do not run fastboot commands for the first ~25 s, or autoboot is cancelled."
