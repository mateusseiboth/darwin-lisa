#!/bin/sh
# Put the ADSP/battery/sensors userland pieces into an iOS 26 root before it is imaged:
#   SYSROOT/fw/adsp.mdt, adsp.b02..b36       ONLY with ADSP_FW_ROOT=1: the audio DSP's firmware (hakios.adsp.c reads
#                                            /fw/<name>), from research/hackios-xnu-kernel/fw-adsp/. By default the
#                                            firmware is built into the kernel (adsp-wire.py step 5): the root has no
#                                            room for its 20 MB before the data volume in super, and wifi-run.sh's
#                                            32 MiB RAM disk copies the root's fw/. Without ADSP_FW_ROOT=1 any adsp.*
#                                            left in SYSROOT/fw is removed.
#   SYSROOT/usr/local/lib/libdlisa-motion.dylib   backboardd's device orientation from kern.hakios.sensors
#   SYSROOT/private/etc/dlisa-inject.conf    + the line "/usr/local/lib/libdlisa-motion.dylib backboardd"
# Usage: sh install-root.sh SYSROOT        (e.g. ~/Dev/mobile/userland-26/out/sysroot-26), then re-image the root.
# Idempotent. Run it AFTER userland/inject/build-inject.sh, which rewrites dlisa-inject.conf from its own copy.
set -eu
SR=${1:?usage: sh install-root.sh SYSROOT}
M=/home/mateusseiboth/Dev/mobile
HERE=$(cd "$(dirname "$0")" && pwd)
FW=$M/research/hackios-xnu-kernel/fw-adsp
MOTION=$M/darwin-lisa/userland/motion
LINE="/usr/local/lib/libdlisa-motion.dylib backboardd"
CONF=$SR/private/etc/dlisa-inject.conf

[ -d "$SR/usr" ] && [ -d "$SR/private/etc" ] || { echo "$SR does not look like an iOS root (no usr/, private/etc/)"; exit 2; }
if [ "${ADSP_FW_ROOT:-0}" = 1 ]; then
	[ -f "$FW/adsp.mdt" ] || python3 -I "$HERE/fw-adsp-stage.py" stage
	python3 -I "$HERE/fw-adsp-stage.py" install "$FW" "$SR/fw"
else
	rm -f "$SR"/fw/adsp.mdt "$SR"/fw/adsp.b[0-9][0-9]
	echo "the ADSP firmware is built into the kernel (adsp-wire.py), not put in $SR/fw (ADSP_FW_ROOT=1 to put it there)"
fi

[ -f "$MOTION/out/libdlisa-motion.dylib" ] || sh "$MOTION/build-motion.sh"
mkdir -p "$SR/usr/local/lib"
install -m 755 "$MOTION/out/libdlisa-motion.dylib" "$SR/usr/local/lib/libdlisa-motion.dylib"
echo "installed $SR/usr/local/lib/libdlisa-motion.dylib"

touch "$CONF"
if grep -qxF "$LINE" "$CONF"; then
	echo "dlisa-inject.conf already has the motion line"
else
	# (keep the file ending in a newline before the new line: init26 turns newlines into the kernel's ";")
	[ -s "$CONF" ] && [ "$(tail -c 1 "$CONF")" != "" ] && echo >> "$CONF"
	echo "$LINE" >> "$CONF"
	echo "dlisa-inject.conf += $LINE"
fi
echo "conf now:"; cat "$CONF"
