#!/bin/bash
# gpu-quietrun.sh TAG "EXTRA ARGS" "T1 T2 T3...": a host run (as tools/run-flashroot.sh) that says nothing to the loader while
# it runs, so the loader's console never scrolls the panel; at each Ti seconds after "oem run": a screenshot
# (logs/TAG-qN.png) and the log (logs/TAG.log). Ends with the orderly stop (oem stop) and its verdict.
set -u
H=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT; HERE=$(cd "$(dirname "$0")" && pwd)
TAG=$1; EXTRA=$2; TIMES=$3
KERNEL=${KERNEL:-kernel-16k-final1-fix-gpustop2.bak}
cd "$H"
cp "$KERNEL" hakios-kernel
UFS=${UFS:-"rd=md0 ufs=2 ufs_part=super ufs_root=1 ufs_write=1 ufs_data=darwin_var var_ufs=1"}
ARGS="loader_return_after=3000 epoch=$(date +%s) sb_delay=8 hv_wdt_pet=1 bootcpu=7 debug=0x8 -v $UFS $EXTRA"
echo "### ${#ARGS} chars: $ARGS"
[ ${#ARGS} -le 460 ] || { echo "too long"; exit 1; }
# from pmOS (or anywhere bootos.sh can reach) to the loader first: "fastboot reboot" with no fastboot device waits for ever
bash "$HERE/../../../tools/bootos.sh" hakios || { echo "no loader"; exit 1; }
fastboot reboot >/dev/null 2>&1 || true
for i in $(seq 1 30); do lsusb 2>/dev/null | grep -q 18d1:d00d || break; sleep 1; done
for i in $(seq 1 60); do timeout 5 fastboot getvar product 2>&1 | grep -qi xnuloader && break; sleep 1; done
timeout 6 fastboot getvar product 2>&1 | grep -qi xnuloader || { echo "loader not up"; exit 1; }
xnuloader/mkpayload.py -o run-payload.bin kernel=hakios-kernel --cmdline "$ARGS" >/dev/null
timeout 60 fastboot stage run-payload.bin >/dev/null 2>&1
timeout 20 fastboot oem clearscreen >/dev/null 2>&1
timeout 20 fastboot oem run >/dev/null 2>&1
T0=$(date +%s)
n=0
for t in $TIMES; do
	n=$((n + 1))
	while [ $(( $(date +%s) - T0 )) -lt "$t" ]; do sleep 1; done
	if timeout 10 fastboot oem screenshot 2>&1 | grep -q OKAY && timeout 60 fastboot get_staged "$S/q.raw" >/dev/null 2>&1; then
		python3 /home/mateusseiboth/Dev/mobile/darwin-lisa/tools/fbraw2png.py "$S/q.raw" "logs/$TAG-q$n.png" 1080 2400 4320
		echo "### +${t}s: logs/$TAG-q$n.png"
	fi
	timeout 5 fastboot oem log >/dev/null 2>&1
	# every snapshot kept (TAG-tSECS.log): the kernel's ring wraps, so the latest alone loses what came before
	timeout 15 fastboot get_staged "$S/q.log" >/dev/null 2>&1 && tr -d '\000' < "$S/q.log" > "logs/$TAG.log" &&
		cp "logs/$TAG.log" "logs/$TAG-t$t.log"
	grep -a -c "no answer" "logs/$TAG.log" | sed 's/^/### no answer lines: /'
done
"$HERE/gpu-endrun.sh" "logs/$TAG-end.log"
