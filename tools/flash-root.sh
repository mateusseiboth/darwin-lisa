#!/usr/bin/env bash
# darwin-lisa: write a COMPLETE root image (userland-26/out/system-26.apfs.full*) into the root region of super from
# pmOS, and nothing else. [WRITES: super bytes 0..4831838208 only]
#
#   PMOS_PASS=... tools/flash-root.sh IMAGE      the phone anywhere bootos.sh can reach; ends in pmOS (not rebooted)
#
# Checks: the image is exactly 4,831,838,208 bytes (the root region; tools/var-write.sh ROOT_MIB) and an APFS container;
# the data region's first 64 MiB (the HFSX data volume) is hashed before and after and must not change; the root region
# is read back and compared with the image's sha256. The password stays in the environment.
set -euo pipefail
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa
IMG=${1:?usage: flash-root.sh IMAGE}
ROOT_BYTES=4831838208
DATA_MIB=64

[[ -n ${PMOS_PASS:-} ]] || { echo "set PMOS_PASS (the pmOS password; it is never written anywhere)" >&2; exit 1; }
command -v sshpass >/dev/null || { echo "sshpass is needed" >&2; exit 1; }
say() { echo "flash-root: $*"; }
export SSHPASS=$PMOS_PASS
SSH=(sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR mateus@172.16.42.1)
SCP=(sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
root_sh() { printf '%s\n' "$PMOS_PASS" | "${SSH[@]}" "sudo -S -p '' sh -c '$1'"; }

size=$(stat -c %s "$IMG")
[[ $size -eq $ROOT_BYTES ]] || { echo "$IMG is $size bytes, not the root region's $ROOT_BYTES: not a complete root image" >&2; exit 1; }
[[ $(dd if="$IMG" bs=4 skip=8 count=1 2>/dev/null) == NXSB ]] || { echo "$IMG is not an APFS container" >&2; exit 1; }
SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
NAME=system-26-${SHA:0:8}.apfs
say "image $IMG, sha256 $SHA"

say "to pmOS"
bash "$D/tools/bootos.sh" pmos
data_before=$(root_sh "dd if=/dev/disk/by-partlabel/super bs=1M skip=4608 count=$DATA_MIB 2>/dev/null | sha256sum" | cut -d' ' -f1)
say "data region (first $DATA_MIB MiB) before: $data_before"

if [[ $(root_sh "test -f /home/mateus/$NAME && sha256sum /home/mateus/$NAME | cut -d\" \" -f1" || true) == "$SHA" ]]; then
	say "the image is on the phone already"
else
	say "copying the root image (4.8 GB) to /home/mateus/$NAME"
	"${SCP[@]}" "$IMG" "mateus@172.16.42.1:/home/mateus/$NAME"
fi
say "writing the root region of super (the data region after it is not touched)"
root_sh "dd if=/home/mateus/$NAME of=/dev/disk/by-partlabel/super bs=1M count=4608 conv=notrunc,fsync 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches"
got=$(root_sh "head -c $ROOT_BYTES /dev/disk/by-partlabel/super | sha256sum" | cut -d' ' -f1)
[[ $got == "$SHA" ]] || { echo "root read-back is $got, not $SHA: the phone stays in pmOS" >&2; exit 1; }
data_after=$(root_sh "dd if=/dev/disk/by-partlabel/super bs=1M skip=4608 count=$DATA_MIB 2>/dev/null | sha256sum" | cut -d' ' -f1)
[[ $data_after == "$data_before" ]] || { echo "the data region changed: $data_before -> $data_after" >&2; exit 1; }
say "root verified ($SHA); data region unchanged; the phone is in pmOS"
