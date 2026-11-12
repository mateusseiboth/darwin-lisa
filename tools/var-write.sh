#!/bin/sh
# darwin-lisa: the data volume (/private/var, HFSX) in its partition of the flash, darwin_var. Runs ON pmOS, as root.
# docs/data-volume.md section 7 (plan B, 2026-11-12) has the whole sequence.
#
#   sudo sh var-write.sh check  IMAGE.gz SHA256          # [READ-ONLY] every check, nothing written
#   sudo VAR_WRITE=yes sh var-write.sh write IMAGE.gz SHA256    # [WRITES: darwin_var, from its block 0]
#   sudo sh var-write.sh verify SHA256 [MIB]             # [READ-ONLY] read the first MIB MiB (default: the image's) back
#   sudo sh var-write.sh backup FILE.gz [MIB]            # [READ-ONLY] save the volume as it is now (rollback)
#
# SHA256 is the raw image's (made by tools/mkvarvol.py, any size up to the partition's; written at block 0). The
# partition is darwin_var, GPT entry 34 of sda, LBA 22228736..30617343 in 4096-byte blocks (8388608 blocks = 32 GiB),
# the same values as loader/data-regions.h. The write is ONE dd into the partition node only (never sda itself), after
# the partition is proved to be darwin_var by its place, size, name and unique GUID. Nothing else of the flash is read
# or written. A volume smaller than the partition is grown to it by init26 with the boot-arg var_grow=1.
#
# Option A (2026-11-10 to 2026-11-12: the volume in super's tail after a 4,831,838,208-byte root) is retired; the
# values it used are in git's history of this file.
set -eu

# ---- the partition (option B) ----------------------------------------------------------------------------------
PARTLABEL=darwin_var
PART_START_512=177829888        # 22228736 * 8: sysfs counts 512-byte sectors
PART_SIZE_512=67108864          # 8388608 * 8 (34,359,738,368 B = 32 GiB)
PART_UUID=7b9f2109-07d5-4cb6-941b-d974a2425d30
PART_MIB=32768
# -----------------------------------------------------------------------------------------------------------------

say() { printf 'var-write: %s\n' "$*"; }
die() { printf 'var-write: STOP: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root (sudo)"
MODE=${1:-}

DEV=$(readlink -f "/dev/disk/by-partlabel/$PARTLABEL") || die "no /dev/disk/by-partlabel/$PARTLABEL"
NAME=$(basename "$DEV")
SYS=/sys/class/block/$NAME
DISK=$(basename "$(readlink -f "$SYS/..")")

prove_partition() {
	[ -b "$DEV" ] || die "$DEV is not a block device"
	[ "$DISK" = sda ] || die "$PARTLABEL is on $DISK, not sda (LUN 0)"
	[ "$NAME" != "$DISK" ] || die "$DEV is a whole disk"
	[ "$(cat "$SYS/start")" = "$PART_START_512" ] || die "$NAME starts at sector $(cat "$SYS/start"), not $PART_START_512"
	[ "$(cat "$SYS/size")" = "$PART_SIZE_512" ] || die "$NAME has $(cat "$SYS/size") sectors, not $PART_SIZE_512"
	[ "$(cat "/sys/class/block/$DISK/queue/logical_block_size")" = 4096 ] || die "$DISK's blocks are not 4096 bytes"
	grep -qx "PARTNAME=$PARTLABEL" "$SYS/uevent" || die "$NAME's GPT name is not $PARTLABEL"
	if command -v blkid >/dev/null 2>&1 && blkid -p -o value -s PART_ENTRY_UUID "$DEV" >/dev/null 2>&1; then
		u=$(blkid -p -o value -s PART_ENTRY_UUID "$DEV")
		[ "$u" = "$PART_UUID" ] || die "$NAME's unique GUID is $u, not $PART_UUID"
	else
		say "no util-linux blkid: unique GUID not checked (place, size and name were)"
	fi
	[ -z "$(ls "$SYS/holders" 2>/dev/null)" ] || die "$NAME has holders: $(ls "$SYS/holders")"
	! grep -q "^$DEV " /proc/mounts || die "$DEV is mounted"
	! grep -q "^$DEV " /proc/swaps 2>/dev/null || die "$DEV is swap"
	say "$DEV is $PARTLABEL: sda, sectors $PART_START_512 + $PART_SIZE_512, blocks of 4096, nothing holds it"
}

# the HFS+ volume header at byte 1024 of the device: its signature, allocation block size (+40) and count (+44)
volume_mib() {    # prints the MiB the volume on $DEV (or $1) occupies, rounded up
	set -- $(dd if="${1:-$DEV}" bs=1024 skip=1 count=1 2>/dev/null | od -An -v -tx1 -N48)
	[ "$1$2" = 4858 ] || [ "$1$2" = 482b ] || die "no HFS Plus volume header (H+/HX) at byte 1024 of ${DEV}"
	bs=$(( 0x${41}${42}${43}${44} )); nb=$(( 0x${45}${46}${47}${48} ))
	echo $(( (bs * nb + 1048575) / 1048576 ))
}

region_sha() { dd if="$DEV" bs=1M count="$1" 2>/dev/null | sha256sum | cut -d' ' -f1; }
drop_cache() { sync; echo 3 >/proc/sys/vm/drop_caches; }

prepare_image() {     # $1 the .gz, $2 its raw sha256 -> $RAW, decompressed beside it and checked; $MIB its size
	[ -f "$1" ] || die "no $1"
	[ -n "${2:-}" ] || die "give the raw image's sha256"
	RAW=${1%.gz}
	[ "$RAW" != "$1" ] || die "$1 is not a .gz"
	if [ ! -f "$RAW" ] || [ "$(sha256sum "$RAW" | cut -d' ' -f1)" != "$2" ]; then
		say "decompressing $1 -> $RAW"
		gzip -dc "$1" >"$RAW.part" && mv "$RAW.part" "$RAW"
	fi
	size=$(stat -c %s "$RAW")
	[ $(( size % 1048576 )) = 0 ] || die "$RAW is $size bytes, not a whole number of MiB"
	MIB=$(( size / 1048576 ))
	[ "$MIB" -le "$PART_MIB" ] || die "$RAW is $MIB MiB, bigger than $PARTLABEL ($PART_MIB MiB)"
	[ "$(sha256sum "$RAW" | cut -d' ' -f1)" = "$2" ] || die "$RAW's sha256 is not $2"
	# HFSX ('HX' at 1024), journaled: what init26 mounts
	set -- $(dd if="$RAW" bs=1024 skip=1 count=1 2>/dev/null | od -An -tx1 -N2)
	[ "$1$2" = 4858 ] || die "$RAW is not an HFSX volume"
	say "$RAW: $MIB MiB HFSX, sha256 ok"
}

case "$MODE" in
check)
	prove_partition
	prepare_image "${2:-}" "${3:-}"
	say "check passed. To write: VAR_WRITE=yes sh $0 write $2 $3"
	;;
write)
	[ "${VAR_WRITE:-}" = yes ] || die "set VAR_WRITE=yes to write"
	prove_partition
	prepare_image "${2:-}" "${3:-}"
	say "WRITING $DEV bytes 0..$(( MIB * 1048576 - 1 ))"
	dd if="$RAW" of="$DEV" bs=1M count="$MIB" conv=notrunc,fsync
	drop_cache
	got=$(region_sha "$MIB")
	[ "$got" = "$3" ] || die "READ BACK DIFFERENT: $got (want $3). Write again or roll back"
	say "written and read back: sha256 $got ($MIB MiB)"
	;;
verify)
	prove_partition
	drop_cache
	MIB=${3:-$(volume_mib)}
	got=$(region_sha "$MIB")
	say "first $MIB MiB: sha256 $got"
	[ -z "${2:-}" ] || [ "$got" = "$2" ] || die "it is not $2"
	;;
backup)
	[ -n "${2:-}" ] || die "give a file name (.gz)"
	prove_partition
	MIB=${3:-$(volume_mib)}
	dd if="$DEV" bs=1M count="$MIB" 2>/dev/null | gzip -1 >"$2"
	say "the volume as it is now ($MIB MiB): $2 ($(stat -c %s "$2") bytes). Restore: gzip -dc $2 | dd of=$DEV bs=1M count=$MIB iflag=fullblock conv=notrunc,fsync"
	;;
*)
	sed -n '2,9p' "$0"
	exit 2
	;;
esac
