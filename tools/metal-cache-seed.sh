#!/usr/bin/env bash
# darwin-lisa: the window server's compiled shader stages from the phone's data volume into the next root image.
#
#   PMOS_PASS=... tools/metal-cache-seed.sh            the phone in pmOS: read the HFSX data volume (the darwin_var
#                                                      partition, read only), take its db/hakios-metal-cache (and tmp/, the
#                                                      fallback of a boot that had no db/ directory) into
#                                                      userland-26/out/sysroot-26/redwood/share/hakios-metal-cache,
#                                                      where HakiosMetalCA.m looks (HM_CACHE_SEED) after its own cache
#                                                      and never writes. Stages seeded before are kept (a union).
#   tools/metal-cache-seed.sh --image FILE              the same from a copy of the data volume already on this PC
#                                                      (the phone's is kept at userland-26/out/darwin-var.hfsx)
#
# Why: the first draw of every new uber shader variant compiles it on CoreAnimation's render thread, 2 to 5 s each on
# the A78; a cold boot pays some 50 of them in the first minute of use (the black Notification Center, the crawling
# app switcher). With the seed the first boot is as warm as the last run.
set -euo pipefail
M=/home/mateusseiboth/Dev/mobile
SEED=$M/userland-26/out/sysroot-26/redwood/share/hakios-metal-cache
VAR_PART=/dev/disk/by-partlabel/darwin_var   # the data volume: its own partition since plan B (loader/data-regions.h)
VAR_OFFSET=0
HOST=mateus@172.16.42.1
# (the copy of the volume is the volume's size, 4 to 32 GiB: the home disk, not /tmp)
S=$(mktemp -d -p "$M/userland-26/out" metal-cache-seed.XXXXXX)
trap 'rm -rf "$S"' EXIT

IMAGE=
if [ "${1:-}" = --image ]; then
	IMAGE=${2:?usage: metal-cache-seed.sh --image FILE}
else
	: "${PMOS_PASS:?PMOS_PASS (the pmOS password) in the environment, never in a file}"
	ssh_pm() { sshpass -p "$PMOS_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$HOST" "$@"; }
	# the HFS+ volume header: 1024 bytes in; block size at +40, block count at +44 (big endian)
	printf '%s\n' "$PMOS_PASS" | ssh_pm "sudo -S dd if=$VAR_PART bs=512 skip=$((VAR_OFFSET / 512 + 2)) count=1 2>/dev/null" > "$S/hdr"
	read -r BLOCK TOTAL < <(python3 -I -c "
import struct,sys
d=open('$S/hdr','rb').read()
sig=d[0:2]
if sig not in (b'H+', b'HX'): sys.exit('not an HFS+ volume header at $VAR_PART+%d: %r' % ($VAR_OFFSET, sig))
bs,tb=struct.unpack('>II', d[40:48]); print(bs, tb)")
	SIZE=$((BLOCK * TOTAL))
	echo "metal-cache-seed: data volume: $TOTAL blocks of $BLOCK = $SIZE bytes; reading it (read only)"
	IMAGE=$M/userland-26/out/darwin-var.hfsx          # kept: read again with --image without the phone
	printf '%s\n' "$PMOS_PASS" | ssh_pm "sudo -S sh -c 'dd if=$VAR_PART bs=1M skip=$((VAR_OFFSET / 1048576)) count=$(( (SIZE + 1048575) / 1048576 )) 2>/dev/null | gzip -1'" | gunzip > "$IMAGE"
	echo "metal-cache-seed: $(stat -c %s "$IMAGE") bytes read"
fi

# 7-Zip reads HFS+/HFSX (a dirty journal is a "Headers Error" it goes past); the volume is named Data and its root is
# /private/var itself, so a path is Data/db/...
mkdir -p "$S/x"
n=0
for d in Data/db/hakios-metal-cache Data/tmp/hakios-metal-cache; do
	7z x -y -o"$S/x" "$IMAGE" "$d" >/dev/null 2>&1 || true
	if [ -d "$S/x/$d" ]; then
		c=$(find "$S/x/$d" -name '*.stage' | wc -l)
		echo "metal-cache-seed: $d: $c stages"
		n=$((n + c))
	fi
done
[ "$n" -gt 0 ] || { echo "metal-cache-seed: the data volume holds no compiled stages (no Metal run since the last boot?)"; exit 1; }
mkdir -p "$SEED"
# (a stage half written when the run ended is a .tmp: never one of the cache's)
find "$S/x" -name '*.stage' -exec cp -n {} "$SEED"/ \;
chmod 644 "$SEED"/*.stage
echo "metal-cache-seed: $(find "$SEED" -name '*.stage' | wc -l) stages in $SEED ($(du -sk "$SEED" | cut -f1) KB); rebuild the root image (FULL=1 userland-26/scripts/mkimage-trim.sh) to carry them"
