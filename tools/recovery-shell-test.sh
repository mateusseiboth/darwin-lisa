#!/usr/bin/env bash
# darwin-lisa: prove U-Boot's "postmarketOS (recovery shell)" entry from the host, with no key pressed, and park the phone
# in pmOS afterwards. Non-interactive: PMOS_PASS from the environment only.
#
#   PMOS_PASS=... tools/recovery-shell-test.sh
#
# From wherever the phone is: an iOS run is ended first (gpu-endrun), then U-Boot's fastboot (bootos.sh uboot) runs the
# entry itself ("fastboot oem run:run pmos_shell"), telnet 172.16.42.1:23 must answer, the kernel command line there
# must carry pmos.debug-shell, "pmos_continue_boot" must then reach pmOS's SSH. Prints RECOVERY_SHELL_OK at the end.
set -uo pipefail
M=/home/mateusseiboth/Dev/mobile
[[ -n ${PMOS_PASS:-} ]] || { echo "set PMOS_PASS" >&2; exit 1; }
say() { echo "recovery-shell-test: $*"; }
case $(bash "$M/darwin-lisa/tools/bootos.sh" state) in
none) say "a kernel may be running: ending the run"; bash "$M/darwin-lisa/kernel/drivers/gpu/gpu-endrun.sh" "$M/research/hackios-xnu-kernel/logs/recovery-test-end.log" ;;
esac
bash "$M/darwin-lisa/tools/bootos.sh" uboot || exit 1
timeout 20 fastboot oem run:"run pmos_shell" 2>&1 | tail -1
say "waiting for the recovery shell's telnet"
python3 -I "$M/darwin-lisa/tools/pmos-debug-shell.py" --timeout 60 --wait 240 'cat /proc/cmdline' 'pmos_continue_boot' | tee /tmp/recovery-shell-test.out
grep -q 'pmos.debug-shell' /tmp/recovery-shell-test.out || { say "the shell came up WITHOUT pmos.debug-shell on the command line"; exit 1; }
bash "$M/darwin-lisa/tools/bootos.sh" pmos || exit 1
echo RECOVERY_SHELL_OK
