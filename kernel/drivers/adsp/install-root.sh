#!/bin/sh
# Put the ADSP/battery/sensors userland pieces into an iOS 26 root before it is imaged:
#   SYSROOT/fw/adsp.mdt, adsp.b02..b36       the audio DSP's firmware (hakios.adsp.c reads /fw/<name>), from
#                                            research/hackios-xnu-kernel/fw-adsp/ (fw-adsp-stage.py stage makes it)
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
[ -f "$FW/adsp.mdt" ] || python3 -I "$HERE/fw-adsp-stage.py" stage
python3 -I "$HERE/fw-adsp-stage.py" install "$FW" "$SR/fw"

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
