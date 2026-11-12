#!/bin/sh
# darwin-lisa: boot iOS 26 with its root on the phone's own flash (the raw `super` partition, written with
# tools/darwin-write.py), with NO resident RAM disk upload: run-fullroot.sh without the 3.84 GB "xl ramdisk".
#
#   darwin-lisa/tools/run-flashroot.sh
#
# The root is chosen by the kernel's UFS driver (kernel/drivers/ufs, tag hakios_ufs): rd=md0 ufs=2 ufs_part=super
# ufs_root=1 (README there: keep rd=md0, the loader only adds it when it uploads a RAM disk). Read only unless the
# arguments ask for the data volume (ufs_write=1 ufs_data=darwin_var var_ufs=1, docs/data-volume.md).
# Environment:
#   KERNEL         the kernel (default kernel26.nopac, in research/hackios-xnu-kernel)
#   XL_SECS        seconds before the kernel returns to the loader (default 1500)
#   XL_POLLS       log polls, about 3 s each (default 500)
#   XL_SHOT_EVERY  a screenshot every N polls (logs/flashroot-shot-<poll>.png; default 60)
#   XL_UFS_ARGS    the root arguments (default above)
#   XL_EXTRA_ARGS  more boot-args (default: the current speed and userland arguments)
#   XL_FW_RAMDISK  a small APFS image uploaded as the resident md0 before the run (the kernel's firmware reader only
#                  looks at md0/md1, not at the flash root): userland-26/out/fw-26-pad32.apfs (fw-26.apfs padded
#                  to 32 MiB). Empty (default): no upload
#   XL_TAG         the name of the log and screenshots (default flashroot: logs/flashroot.log, flashroot-shot-N.png)
#   XL_VBAT_MIN    mV: when the battery driver's last VBAT line is below it, the run is stopped (default 3700;
#                  the phone does not charge while iOS runs). 0 = never
# Logs: research/hackios-xnu-kernel/logs/$XL_TAG.log (the biggest live snapshot). The script itself writes nothing to
# storage; with ufs_write=1 the kernel writes the data region.
set -e
H=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel
cd "$H"
KERNEL="${KERNEL:-kernel26.nopac}"
[ -f "$KERNEL" ] || { echo "no kernel ($KERNEL)"; exit 1; }
SECS="${XL_SECS:-1500}"
UFS="${XL_UFS_ARGS:-rd=md0 ufs=2 ufs_part=super ufs_root=1}"
EXTRA="${XL_EXTRA_ARGS-display=1 console_open=1 keystore=1 wlan_stub=1 init_run=/usr/libexec/usermanagerd:--init init_run_secs=60 cpufreq=1 cpufreq_max=2400 cpus=7}"
SHOT="${XL_SHOT_EVERY:-60}"
TAG="${XL_TAG:-flashroot}"
VMIN="${XL_VBAT_MIN:-3700}"
PHONE=${XDG_RUNTIME_DIR:-/tmp}/darwin-hakios-phone
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$KERNEL" hakios-kernel

echo "### fresh loader session (no resident image in it)"
fastboot reboot >/dev/null 2>&1 || true
for i in $(seq 1 30); do lsusb 2>/dev/null | grep -q 18d1:d00d || break; sleep 1; done
for i in $(seq 1 60); do timeout 5 fastboot getvar product 2>&1 | grep -qi xnuloader && break; sleep 1; done
timeout 6 fastboot getvar product 2>&1 | grep -qi xnuloader || { echo "loader not up"; exit 1; }
rm -f "$PHONE.ran"

if [ -n "${XL_FW_RAMDISK:-}" ]; then
	echo "### resident md0: $XL_FW_RAMDISK ($(sha256sum "$XL_FW_RAMDISK" | cut -c1-8))"
	XL_RAMDISK=resident ./xl ramdisk "$XL_FW_RAMDISK" || { echo "resident upload failed"; exit 2; }
fi
T0=$(date +%s)

ARGS="loader_return_after=$SECS epoch=$(date +%s) sb_delay=8 hv_wdt_pet=1 bootcpu=7 debug=0x8 -v $UFS $EXTRA"
echo "### boot $KERNEL ($(sha256sum "$KERNEL" | cut -c1-8)), root from flash: $ARGS"
xnuloader/mkpayload.py -o run-payload.bin kernel=hakios-kernel --cmdline "$ARGS" >/dev/null
timeout 60 fastboot stage run-payload.bin >/dev/null 2>&1
timeout 20 fastboot oem run >/dev/null 2>&1

best=0
for i in $(seq 1 "${XL_POLLS:-500}"); do
	timeout 5 fastboot oem log >/dev/null 2>&1 || true
	if timeout 8 fastboot get_staged "$TMP/fr" >/dev/null 2>&1; then
		n=$(tr -d '\000' <"$TMP/fr" | wc -c)
		[ "$n" -ge "$best" ] && best=$n && tr -d '\000' <"$TMP/fr" >"logs/$TAG.log"
	fi
	if [ $((i % SHOT)) -eq 0 ]; then
		geom=$(timeout 5 fastboot getvar screen 2>&1 | sed -n 's/^screen: //p')
		if [ -n "$geom" ] && timeout 10 fastboot oem screenshot 2>&1 | grep -q OKAY &&
			timeout 60 fastboot get_staged "$TMP/shot.raw" >/dev/null 2>&1; then
			python3 /home/mateusseiboth/Dev/mobile/darwin-lisa/tools/fbraw2png.py "$TMP/shot.raw" \
				"logs/$TAG-shot-$i.png" $geom && echo "### poll $i ($(date +%T), +$(( $(date +%s) - T0 ))s): logs/$TAG-shot-$i.png"
		fi
		cp "logs/$TAG.log" "logs/$TAG-at-$i.log" 2>/dev/null || true	# the ring may wrap: keep each stage
		grep -a '\[MEM\]' "logs/$TAG.log" | tail -1 || true
		grep -a 'battery: VBAT '"logs/$TAG.log" | tail -1 | cut -c1-120 || true
	fi
	v=$(grep -a 'battery: VBAT_SNS' "logs/$TAG.log" 2>/dev/null | tail -1 | sed -n 's/.*-> \([0-9]*\) mV.*/\1/p')
	if [ "$VMIN" -gt 0 ] && [ -n "$v" ] && [ "$v" -lt "$VMIN" ]; then
		echo "### VBAT $v mV is below $VMIN: stopping the run (charge in pmOS)"
		timeout 10 fastboot oem stop >/dev/null 2>&1 || true
		break
	fi
	r=$(timeout 5 fastboot getvar result 2>&1 | grep -i '^result:' || true)
	case "$r" in *returned* | *exception* | *checkpoint* | *hung*) echo "### $r"; break ;; esac
	sleep 2
done
echo "---- root-mount markers ----"
grep -anE "ufs:|BSD root|apfs: volume|apfs: device|panic|Debugger" "logs/$TAG.log" | head -40
echo "---- free memory ----"
grep -a '\[MEM\]' "logs/$TAG.log" | tail -5
