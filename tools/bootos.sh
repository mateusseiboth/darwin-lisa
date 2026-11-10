#!/usr/bin/env bash
# darwin-lisa: choose the phone's OS from the host over USB, with no button pressed.
#
#   tools/bootos.sh state                 where the phone is now: loader, uboot-fastboot, abl-fastboot, ums, pmos, none
#   tools/bootos.sh pmos                  boot postmarketOS and wait until its SSH answers
#   tools/bootos.sh hakios                back to the HakiOS loader (fastboot product "xnuloader")
#   tools/bootos.sh ums                   the phone's UFS LUN 0 as a USB disk on this PC; prints /dev/sdX
#   tools/bootos.sh uboot                 U-Boot's own fastboot (for "fastboot oem run:...")
#   tools/bootos.sh menu                  U-Boot's boot menu, without the countdown
#
# How: the loader takes "fastboot oem bootos:TARGET" (darwin-lisa/loader/oem-bootos.patch), stops its USB function
# and watchdog and exits back to U-Boot, which runs TARGET (u-boot-lisa/board/qualcomm/hakios-lisa.env, osel_check).
# Without that request U-Boot behaves as before: menu, 5 s, HakiOS. Rebooting pmOS lands in HakiOS again.
# Needs the loader with the patch (vendor_boot_a) and U-Boot with osel_check (boot_a); with an older one,
# "oem bootos:" answers FAIL "unknown command" and nothing happens.
#
# pmOS access: SSH to $PMOS_USER@$PMOS_HOST (mateus@172.16.42.1). Use an SSH key, or put the password in the
# environment as PMOS_PASS (used through sshpass -e and sudo -S; never written anywhere). sudo is needed for reboot.
# Nothing here writes to the phone's storage.
set -euo pipefail

PMOS_HOST=${PMOS_HOST:-172.16.42.1}
PMOS_USER=${PMOS_USER:-mateus}
FASTBOOT=${FASTBOOT:-fastboot}
WAIT_LOADER=${WAIT_LOADER:-90}        # seconds: a reset, U-Boot's 5 s menu and the loader take about 15
WAIT_PMOS=${WAIT_PMOS:-180}
WAIT_UMS=${WAIT_UMS:-60}

say() { printf 'bootos: %s\n' "$*" >&2; }
die() { say "$*"; exit 1; }

# fastboot with a short timeout; prints the command's whole output (fastboot writes answers to stderr)
fb() { timeout "${FB_TIMEOUT:-15}" "$FASTBOOT" "$@" 2>&1; }

fb_product() {
	local out
	out=$(fb getvar product) || return 1
	sed -n 's/^product: *//p' <<<"$out" | head -1
}

ssh_opts=(-o ConnectTimeout=4 -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
pmos_ssh() {
	if [[ -n ${PMOS_PASS:-} ]]; then
		command -v sshpass >/dev/null || die "PMOS_PASS is set but sshpass is not installed"
		SSHPASS=$PMOS_PASS sshpass -e ssh "${ssh_opts[@]}" "$PMOS_USER@$PMOS_HOST" "$@"
	else
		ssh "${ssh_opts[@]}" -o BatchMode=yes "$PMOS_USER@$PMOS_HOST" "$@"
	fi
}

# the ums disk: U-Boot's mass-storage gadget answers INQUIRY as vendor "Linux", model "UMS disk 0" (the name
# cmd/usb_mass_storage.c gives the LUN; f_mass_storage's "File-Store Gadget" string is never sent)
ums_disk() {
	local d
	for d in /sys/block/sd*; do
		[[ -e $d/device/vendor && -e $d/device/model ]] || continue
		if [[ $(tr -d ' ' <"$d/device/vendor") == Linux && $(sed 's/ *$//' <"$d/device/model") == "UMS disk 0" ]]; then
			echo "/dev/${d##*/}"
			return 0
		fi
	done
	return 1
}

state() {
	local p
	if ums_disk >/dev/null; then echo ums; return; fi
	if p=$(fb_product) && [[ -n $p ]]; then
		case $p in
		xnuloader) echo loader ;;
		lisa) echo abl-fastboot ;;
		*) echo uboot-fastboot ;;
		esac
		return
	fi
	if timeout 6 bash -c "exec 3<>/dev/tcp/$PMOS_HOST/22" 2>/dev/null; then echo pmos; return; fi
	echo none
}

wait_for() {    # wait_for SECONDS WHAT: until state is WHAT
	local limit=$1 want=$2 t=0
	while ((t < limit)); do
		[[ $(state) == "$want" ]] && return 0
		sleep 2
		t=$((t + 2))
	done
	return 1
}

wait_pmos_ssh() {
	local t=0
	while ((t < WAIT_PMOS)); do
		if pmos_ssh true 2>/dev/null; then return 0; fi
		sleep 3
		t=$((t + 3))
	done
	return 1
}

# The phone sits in pmOS to charge between runs. Once it suspends, USB loses its link and nothing reaches it until
# someone presses Power, so hold a sleep inhibitor for as long as pmOS runs (the reboot to the loader ends it).
# The inhibitor runs as its own transient unit: a nohup'd child of the SSH session is killed when the session closes.
hold_awake() {
	local cmd='systemctl stop darwin-lisa-awake 2>/dev/null; systemctl reset-failed darwin-lisa-awake 2>/dev/null; systemd-run --quiet --collect --unit=darwin-lisa-awake systemd-inhibit --what=sleep:idle:handle-lid-switch --who=darwin-lisa --why=parked-by-bootos --mode=block sleep infinity'
	if [[ -n ${PMOS_PASS:-} ]]; then
		printf '%s\n' "$PMOS_PASS" | pmos_ssh "sudo -S -p '' sh -c '$cmd'" || say "warning: could not hold pmOS awake"
	else
		pmos_ssh "sudo -n sh -c '$cmd'" || say "warning: could not hold pmOS awake (no PMOS_PASS)"
	fi
	sleep 1
	pmos_ssh 'systemd-inhibit --list --no-pager | grep -q darwin-lisa' ||
		die "warning: could not hold pmOS awake (no darwin-lisa inhibitor listed)"
	say "pmOS held awake (sleep inhibitor until the next reboot)"
}

# from wherever the phone is, to the HakiOS loader
to_loader() {
	local s
	s=$(state)
	say "phone is in: $s"
	case $s in
	loader) return 0 ;;
	pmos)
		if [[ -n ${PMOS_PASS:-} ]]; then
			printf '%s\n' "$PMOS_PASS" | pmos_ssh 'sudo -S -p "" systemctl reboot 2>/dev/null || sudo -S -p "" reboot' || true
		else
			pmos_ssh 'sudo -n reboot' || die "pmOS: sudo needs a password (set PMOS_PASS) or a NOPASSWD rule"
		fi ;;
	ums)
		local d
		d=$(ums_disk)
		sync
		eject "$d" 2>/dev/null || sudo eject "$d" || die "could not eject $d: press a key on the phone to end ums"
		;;
	uboot-fastboot | abl-fastboot) fb reboot >/dev/null || true ;;
	none) say "phone not seen (off, or in a kernel run): waiting for the loader" ;;
	esac
	wait_for "$WAIT_LOADER" loader || die "the loader did not come up within ${WAIT_LOADER}s"
	say "loader is up"
}

# ask a fresh loader for TARGET; a loader that has run a kernel is rebooted first
ask_loader() {
	local target=$1 out
	to_loader
	out=$(fb oem "bootos:$target") || true
	if grep -q 'a kernel is running' <<<"$out"; then
		say "a kernel is running: asking it to stop"
		fb oem stop >/dev/null || true
		sleep 5
	fi
	if grep -q 'a kernel ran\|a kernel is running' <<<"$out"; then
		say "a kernel ran in this loader session: rebooting it first"
		fb reboot >/dev/null || true
		sleep 5
		wait_for "$WAIT_LOADER" loader || die "the loader did not come back"
		out=$(fb oem "bootos:$target") || true
	fi
	grep -q 'OKAY' <<<"$out" || die "the loader refused: $(tr '\n' ' ' <<<"$out")"
	say "loader accepted bootos:$target"
}

target=${1:-}
case $target in
state) state ;;
hakios) to_loader ;;
pmos)
	if [[ $(state) == pmos ]] && pmos_ssh true 2>/dev/null; then say "already in pmOS"; hold_awake; exit 0; fi
	ask_loader pmos
	wait_pmos_ssh || die "pmOS SSH did not answer within ${WAIT_PMOS}s"
	say "pmOS is up: $PMOS_USER@$PMOS_HOST"
	hold_awake
	;;
ums)
	if d=$(ums_disk); then echo "$d"; exit 0; fi
	ask_loader ums
	t=0
	until d=$(ums_disk); do
		# a ums that ended without an eject leaves U-Boot's fastboot up, with the reason (hakios-lisa.env ums_disk)
		if [[ $(state) == uboot-fastboot ]]; then
			die "ums ended: $(fb getvar ums | sed -n 's/^ums: *//p' | head -1) (U-Boot fastboot is up: fastboot oem console, fastboot reboot)"
		fi
		((t < WAIT_UMS)) || die "no USB disk within ${WAIT_UMS}s"
		sleep 2
		t=$((t + 2))
	done
	say "UFS LUN 0 is $d (eject it to restart the phone into the menu/HakiOS)"
	echo "$d"
	;;
uboot)
	ask_loader uboot
	wait_for 30 uboot-fastboot || die "U-Boot's fastboot did not come up"
	say "U-Boot fastboot is up (fastboot oem run:CMD, fastboot reboot)"
	;;
menu) ask_loader menu ;;
*) sed -n '2,12s/^# \{0,1\}//p' "$0"; exit 2 ;;
esac
