#!/bin/sh
# darwin-lisa: host test of power/nosleep-wire.py. Wires a copy of the pristine IOPMrootDomain.cpp twice (the second
# run must be all [skip]) and checks the result: system sleep refused by default, syssleep=1 gives it back, and the
# device tree's sleep-disabled can no longer clear the default.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WIRE=$HERE/../nosleep-wire.py
PRISTINE=${PRISTINE:-/home/mateusseiboth/Dev/mobile/research/UniXNU/out/feeds/xnu-3199e8bc/iokit/Kernel/IOPMrootDomain.cpp}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/iokit/Kernel"
cp "$PRISTINE" "$T/iokit/Kernel/IOPMrootDomain.cpp"
F=$T/iokit/Kernel/IOPMrootDomain.cpp
fails=0
check() {    # check WHAT COMMAND...
	what=$1
	shift
	if "$@"; then echo "  ok   $what"; else echo "  FAIL $what"; fails=$((fails + 1)); fi
}

first=$(XNU_WORK=$T python3 -I "$WIRE")
second=$(XNU_WORK=$T python3 -I "$WIRE")
check "the first run applies every edit" sh -c "! printf '%s\n' \"\$1\" | grep -q -e '\[skip\]' -e '\[FAIL\]'" _ "$first"
check "the second run only skips" sh -c "! printf '%s\n' \"\$1\" | grep -q -e '\[ok\]' -e '\[FAIL\]'" _ "$second"
check "sleep is refused by default" grep -q '^static uint32_t         gSleepDisabledFlag = 1;' "$F"
check "the pristine default is gone" sh -c "! grep -q '^static uint32_t         gSleepDisabledFlag = 0;' \"\$1\"" _ "$F"
check "syssleep=1 gives sleep back" grep -q 'PE_parse_boot_argn("syssleep", &syssleep, sizeof(syssleep)) && syssleep' "$F"
check "syssleep is read in start()" sh -c "grep -n 'PE_parse_boot_argn(\"noidle\"' \"\$1\" | head -1 | cut -d: -f1 > \"\$2/a\";
	grep -n '\"syssleep\"' \"\$1\" | head -1 | cut -d: -f1 > \"\$2/b\"; [ \$(cat \"\$2/b\") -gt \$(cat \"\$2/a\") ]" _ "$F" "$T"
check "the device tree cannot clear the default" grep -q 'dlisa_dt_sleep_disabled' "$F"
check "one syssleep parse only" sh -c "[ \$(grep -c '\"syssleep\"' \"\$1\") -eq 1 ]" _ "$F"

[ $fails -eq 0 ] && echo "nosleep-wire: all checks pass" || { echo "nosleep-wire: $fails check(s) failed"; exit 1; }
