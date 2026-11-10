#!/usr/bin/env bash
# darwin-lisa: the PMK8350 clock's offset to UTC, measured from pmOS, for boot-arg rtc_offset= (kernel/drivers/rtc).
#
#   tools/rtc-offset.sh            prints "rtc_offset=N" (and how it was measured on stderr)
#
# pmOS's rtc0 on lisa is the same PMIC counter, bare: its DT has neither allow-set-time nor an nvmem offset, so
# /sys/class/rtc/rtc0/since_epoch is the count the iOS kernel reads. The offset is this PC's UTC (keep it on NTP)
# minus that count. The counter runs while the battery is in, so the offset holds across reboots and both OSes; run
# it once, boot iOS with rtc=1 rtc_offset=N, and init26 keeps it on the data volume for the boots after.
# Needs the phone in pmOS (tools/bootos.sh pmos). SSH as bootos.sh does: a key, or PMOS_PASS in the environment.
# Reads only.
set -euo pipefail

PMOS_HOST=${PMOS_HOST:-172.16.42.1}
PMOS_USER=${PMOS_USER:-mateus}
ssh_opts=(-o ConnectTimeout=4 -o StrictHostKeyChecking=accept-new)

pmos_ssh() {
	if [[ -n ${PMOS_PASS:-} ]]; then
		SSHPASS=$PMOS_PASS sshpass -e ssh "${ssh_opts[@]}" "$PMOS_USER@$PMOS_HOST" "$@"
	else
		ssh "${ssh_opts[@]}" -o BatchMode=yes "$PMOS_USER@$PMOS_HOST" "$@"
	fi
}

before=$(date -u +%s)
read -r name count < <(pmos_ssh 'printf "%s %s\n" "$(cat /sys/class/rtc/rtc0/name)" "$(cat /sys/class/rtc/rtc0/since_epoch)"')
after=$(date -u +%s)
[[ $count =~ ^[0-9]+$ ]] || { echo "rtc-offset: rtc0 gave no count ($name $count)" >&2; exit 1; }
now=$(( (before + after) / 2 ))
echo "rtc-offset: pmOS rtc0 ($name) counts $count s; this PC says $now ($(date -u -d "@$now" '+%F %T') UTC), \
read within $(( after - before )) s" >&2
echo "rtc_offset=$(( now - count ))"
