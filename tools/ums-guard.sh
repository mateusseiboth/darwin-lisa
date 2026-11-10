#!/usr/bin/env bash
# darwin-lisa: make the phone's UFS LUN 0, exposed on this PC by U-Boot's USB disk mode (tools/bootos.sh ums),
# read-only in this PC's kernel, every partition of it, except the ones named on the command line.
#
#   sudo tools/ums-guard.sh /dev/sdX            all of it read-only (the GPT dumps, lpdump, the inner pmOS table)
#   sudo tools/ums-guard.sh /dev/sdX 20         only partition 20 (super) writable; the disk itself stays writable
#                                               because Linux refuses writes to every partition of a read-only disk
#
# Also tells udisks to leave it alone (no automount: a desktop that mounts persist, cust or the pmOS partitions would
# replay their journals, which is a write), through a udev rule under /run that is gone at the next boot of the PC.
set -euo pipefail

disk=${1:?usage: ums-guard.sh /dev/sdX [PARTNUM...]}
shift
name=${disk##*/}
[[ $(tr -d ' ' <"/sys/block/$name/device/vendor") == Linux &&
	$(sed 's/ *$//' <"/sys/block/$name/device/model") == "File-Store Gadget" ]] ||
	{ echo "ums-guard: $disk is not U-Boot's USB disk (vendor Linux, model File-Store Gadget)" >&2; exit 1; }

rule=/run/udev/rules.d/99-lisa-ums-ignore.rules
if [[ ! -e $rule ]]; then
	mkdir -p /run/udev/rules.d
	echo 'SUBSYSTEM=="block", ATTRS{vendor}=="Linux*", ATTRS{model}=="File-Store Gadget*", ENV{UDISKS_IGNORE}="1", ENV{UDISKS_AUTO}="0"' >"$rule"
	udevadm control --reload
	udevadm trigger --subsystem-match=block --action=change
fi
for m in $(lsblk -nrpo NAME,MOUNTPOINT "$disk" | awk '$2 != "" {print $1}'); do
	echo "ums-guard: $m is MOUNTED on this PC: unmount it first" >&2
	exit 1
done

writable=" $* "
any=0
for p in /sys/block/"$name"/"$name"*; do
	n=${p##*/}
	num=${n#"$name"}
	num=${num#p}
	if [[ $writable == *" $num "* ]]; then
		blockdev --setrw "/dev/$n"
		any=1
		echo "ums-guard: /dev/$n writable"
	else
		blockdev --setro "/dev/$n"
	fi
done
if ((any)); then blockdev --setrw "$disk"; else blockdev --setro "$disk"; fi
echo "ums-guard: $disk $( ((any)) && echo "writable only through partition(s) $*" || echo "read-only, all of it")"
