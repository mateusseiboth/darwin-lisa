#!/usr/bin/env bash
# darwin-lisa: write a vendor_boot image (out/vendor_boot-lisa-NAME.img, loader/build-combined.sh) to vendor_boot_a from
# pmOS, with a backup of what was there. [WRITES: vendor_boot_a only]
#
#   PMOS_PASS=... tools/flash-vendorboot.sh IMAGE      the phone anywhere bootos.sh can reach; ends in pmOS (not rebooted)
#
# Checks: the image is an Android boot image ("VNDRBOOT") no bigger than the partition; the partition's first
# IMAGE-size bytes are saved to backups/vendor_boot_a-before-<date>-<sha8>.img on this PC before the write; the write is
# read back and compared with the image's sha256. The password stays in the environment. Rollback: this script with the
# backup image (or tools/restore-safe.sh for the restore-safe set).
set -euo pipefail
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa
IMG=${1:?usage: flash-vendorboot.sh IMAGE}
PART=/dev/disk/by-partlabel/vendor_boot_a

[[ -n ${PMOS_PASS:-} ]] || { echo "set PMOS_PASS (the pmOS password; it is never written anywhere)" >&2; exit 1; }
command -v sshpass >/dev/null || { echo "sshpass is needed" >&2; exit 1; }
say() { echo "flash-vendorboot: $*"; }
export SSHPASS=$PMOS_PASS
SSH=(sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR mateus@172.16.42.1)
SCP=(sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
root_sh() { printf '%s\n' "$PMOS_PASS" | "${SSH[@]}" "sudo -S -p '' sh -c '$1'"; }

[[ $(dd if="$IMG" bs=8 count=1 2>/dev/null) == VNDRBOOT ]] || { echo "$IMG is not a vendor_boot image" >&2; exit 1; }
size=$(stat -c %s "$IMG")
SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
NAME=vendor_boot-${SHA:0:8}.img
say "image $IMG, $size bytes, sha256 $SHA"

say "to pmOS"
bash "$D/tools/bootos.sh" pmos
part_bytes=$(root_sh "blockdev --getsize64 $PART")
[[ $size -le $part_bytes ]] || { echo "$IMG ($size) is bigger than vendor_boot_a ($part_bytes)" >&2; exit 1; }

mkdir -p "$M/backups"
BACKUP=$M/backups/vendor_boot_a-before-$(date +%Y%m%d-%H%M)-${SHA:0:8}.img
say "saving the partition's first $size bytes to $BACKUP"
root_sh "head -c $size $PART > /home/mateus/vendor_boot_a-before.img"
"${SCP[@]}" "mateus@172.16.42.1:/home/mateus/vendor_boot_a-before.img" "$BACKUP"
say "backup sha256 $(sha256sum "$BACKUP" | cut -d' ' -f1)"

say "copying the image to /home/mateus/$NAME"
"${SCP[@]}" "$IMG" "mateus@172.16.42.1:/home/mateus/$NAME"
[[ $(root_sh "sha256sum /home/mateus/$NAME | cut -d\" \" -f1") == "$SHA" ]] || { echo "the copy on the phone does not match" >&2; exit 1; }
say "writing vendor_boot_a"
root_sh "dd if=/home/mateus/$NAME of=$PART bs=1M conv=fsync 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches"
got=$(root_sh "head -c $size $PART | sha256sum" | cut -d' ' -f1)
[[ $got == "$SHA" ]] || { echo "read-back is $got, not $SHA: restore $BACKUP with this script" >&2; exit 1; }
say "vendor_boot_a verified ($SHA); the phone is in pmOS"
