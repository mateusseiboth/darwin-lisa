#!/usr/bin/env bash
# darwin-lisa: put lisa in Xiaomi's ABL fastboot, in the mode that takes transfers, with no key pressed.
#
#   tools/abl-fastboot.sh            from pmOS, the HakiOS loader, U-Boot or ABL fastboot; exits 0 when ABL fastboot
#                                    has accepted a harmless 4 KiB download (fastboot stage: RAM only, nothing flashed)
#
# What Xiaomi's ABL does on lisa: the first "reboot bootloader" lands in a fastboot that REFUSES transfers; a second
# "fastboot reboot bootloader" from inside that one gives a fastboot that accepts boot/flash. ABL's fastboot is only
# reachable through a "bootloader" reboot reason, which pmOS can set (reboot bootloader) and the HakiOS loader and
# U-Boot cannot (their reset carries no reason), so from the loader the way goes through pmOS (tools/bootos.sh pmos).
# If a step fails, the fallback is the user: Power + Volume Down with the phone off.
# Nothing here writes to the phone's storage. The pmOS password, if needed, comes from PMOS_PASS (never stored).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
FASTBOOT=${FASTBOOT:-fastboot}
PMOS_HOST=${PMOS_HOST:-172.16.42.1}
PMOS_USER=${PMOS_USER:-mateus}

say() { printf 'abl-fastboot: %s\n' "$*" >&2; }
fb() { timeout "${FB_TIMEOUT:-20}" "$FASTBOOT" "$@" 2>&1; }

product() { fb getvar product | sed -n 's/^product: *//p' | head -1; }

wait_abl() {    # until ABL's fastboot (product lisa) answers
	local t=0
	while ((t < ${1:-90})); do
		[[ $(product || true) == lisa ]] && return 0
		sleep 2
		t=$((t + 2))
	done
	return 1
}

accepts_transfers() {
	local probe out
	probe=$(mktemp)
	head -c 4096 /dev/zero >"$probe"
	out=$(fb stage "$probe") || true
	rm -f "$probe"
	say "stage probe: $(tr '\n' ' ' <<<"$out")"
	grep -q 'OKAY' <<<"$out" && ! grep -qi 'FAIL\|refus\|not allowed\|error' <<<"$out"
}

fallback() {
	say "$*"
	say "USER NEEDED: power the phone off, then hold Power + Volume Down until the fastboot screen."
	exit 1
}

s=$("$HERE/bootos.sh" state)
say "phone is in: $s"
case $s in
abl-fastboot) ;;
pmos | loader | uboot-fastboot | ums | none)
	[[ $s == pmos ]] || "$HERE/bootos.sh" pmos
	say "pmOS: reboot bootloader"
	ssh_opts=(-o ConnectTimeout=4 -o StrictHostKeyChecking=accept-new)
	if [[ -n ${PMOS_PASS:-} ]]; then
		printf '%s\n' "$PMOS_PASS" | SSHPASS=$PMOS_PASS sshpass -e ssh "${ssh_opts[@]}" "$PMOS_USER@$PMOS_HOST" \
			'sudo -S -p "" reboot bootloader' || true
	else
		ssh "${ssh_opts[@]}" -o BatchMode=yes "$PMOS_USER@$PMOS_HOST" 'sudo -n reboot bootloader' || true
	fi
	wait_abl 120 || fallback "ABL fastboot did not come up after pmOS's reboot bootloader"
	;;
esac

if accepts_transfers; then
	say "ABL fastboot takes transfers"
	exit 0
fi
say "ABL fastboot refuses transfers: fastboot reboot bootloader once more"
fb reboot bootloader >/dev/null || true
sleep 4
wait_abl 90 || fallback "ABL fastboot did not come back after the second reboot bootloader"
accepts_transfers || fallback "ABL fastboot still refuses transfers"
say "ABL fastboot takes transfers"
