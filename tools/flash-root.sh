#!/usr/bin/env bash
# darwin-lisa: write a COMPLETE root image (userland-26/out/system-26.apfs.full*) into super from pmOS, and nothing
# else. [WRITES: super bytes 0..IMAGE SIZE only]
#
#   PMOS_PASS=... tools/flash-root.sh IMAGE      the phone anywhere bootos.sh can reach; ends in pmOS (not rebooted)
#
# Since plan B (2026-11-12, docs/data-volume.md section 7) super is the root's alone: the data volume is the darwin_var
# partition. Checks: the image is an APFS container, a whole number of MiB (mkimage-trim.sh pads to 32 MiB) and no bigger
# than super (9,126,805,504 bytes); the data volume's first 64 MiB (darwin_var) are hashed before and after and must not
# change; the written range is read back and compared with the image's sha256. The password stays in the environment.
set -euo pipefail
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa
IMG=${1:?usage: flash-root.sh IMAGE}
SUPER_BYTES=9126805504
VAR_PART=/dev/disk/by-partlabel/darwin_var
DATA_MIB=64

[[ -n ${PMOS_PASS:-} ]] || { echo "set PMOS_PASS (the pmOS password; it is never written anywhere)" >&2; exit 1; }
command -v sshpass >/dev/null || { echo "sshpass is needed" >&2; exit 1; }
say() { echo "flash-root: $*"; }
export SSHPASS=$PMOS_PASS
SSH=(sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR mateus@172.16.42.1)
SCP=(sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
root_sh() { printf '%s\n' "$PMOS_PASS" | "${SSH[@]}" "sudo -S -p '' sh -c '$1'"; }

size=$(stat -c %s "$IMG")
[[ $size -le $SUPER_BYTES ]] || { echo "$IMG is $size bytes, bigger than super ($SUPER_BYTES)" >&2; exit 1; }
[[ $((size % 1048576)) -eq 0 ]] || { echo "$IMG is $size bytes, not a whole number of MiB" >&2; exit 1; }
[[ $(dd if="$IMG" bs=4 skip=8 count=1 2>/dev/null) == NXSB ]] || { echo "$IMG is not an APFS container" >&2; exit 1; }
MIB=$((size / 1048576))
SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
NAME=system-26-${SHA:0:8}.apfs
say "image $IMG, $size bytes ($MIB MiB), sha256 $SHA"

say "to pmOS"
bash "$D/tools/bootos.sh" pmos
root_sh "test -b $VAR_PART" || { echo "no $VAR_PART on the phone: plan B's carve is not there" >&2; exit 1; }
data_before=$(root_sh "dd if=$VAR_PART bs=1M count=$DATA_MIB 2>/dev/null | sha256sum" | cut -d' ' -f1)
say "data volume (first $DATA_MIB MiB of darwin_var) before: $data_before"

if [[ $(root_sh "test -f /home/mateus/$NAME && sha256sum /home/mateus/$NAME | cut -d\" \" -f1" || true) == "$SHA" ]]; then
	say "the image is on the phone already"
else
	say "copying the root image ($MIB MiB) to /home/mateus/$NAME"
	"${SCP[@]}" "$IMG" "mateus@172.16.42.1:/home/mateus/$NAME"
fi
say "writing super bytes 0..$size (the rest of super and darwin_var are not touched)"
root_sh "dd if=/home/mateus/$NAME of=/dev/disk/by-partlabel/super bs=1M count=$MIB conv=notrunc,fsync 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches"
got=$(root_sh "head -c $size /dev/disk/by-partlabel/super | sha256sum" | cut -d' ' -f1)
[[ $got == "$SHA" ]] || { echo "root read-back is $got, not $SHA: the phone stays in pmOS" >&2; exit 1; }
data_after=$(root_sh "dd if=$VAR_PART bs=1M count=$DATA_MIB 2>/dev/null | sha256sum" | cut -d' ' -f1)
[[ $data_after == "$data_before" ]] || { echo "the data volume changed: $data_before -> $data_after" >&2; exit 1; }
say "root verified ($SHA); data volume unchanged; the phone is in pmOS (the staging copy /home/mateus/$NAME stays there: remove it when space is short)"
