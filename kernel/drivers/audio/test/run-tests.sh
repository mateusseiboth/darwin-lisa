#!/bin/sh
# The speakers' host tests: the TFA9873 status decoding and fault rules (C, dlisa.tfa9873.flags.h) and the tuning taken
# from the stock container (tfa-cnt.py; skipped when the stock tfa98xx.cnt or NXP's field names are not on this host),
# and that dlisa.tfa9873.tuning.h is what tfa-cnt.py makes of them. Nothing here needs the phone or docker.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
M=$HOME/Dev/mobile
OUT=${TMPDIR:-/tmp}/dlisa-audio-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
fail=0
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/tfa_flags_test" "$HERE/tfa_flags_test.c" && "$OUT/tfa_flags_test" || fail=1
python3 -I "$HERE/test_tfa_cnt.py" -q 2>&1 | tail -n 1 || fail=1
python3 -I "$HERE/test_tfa_cnt.py" -q >/dev/null 2>&1 || { echo "FAIL: test_tfa_cnt.py"; fail=1; }
CNT=$M/stock/super/vendor/firmware/tfa98xx.cnt
NAMES=$M/research/tfa98xx-lisa-r-oss/techpack/audio/asoc/codecs/tfa98xx/inc/tfa9873_tfafieldnames_B0.h
if [ -f "$CNT" ] && [ -f "$NAMES" ]; then
	python3 -I "$HERE/../tfa-cnt.py" header "$CNT" stereo "$NAMES" > "$OUT/tuning.h"
	cmp -s "$OUT/tuning.h" "$HERE/../dlisa.tfa9873.tuning.h" && echo "dlisa.tfa9873.tuning.h: as generated" ||
	    { echo "FAIL: dlisa.tfa9873.tuning.h differs from tfa-cnt.py's output"; fail=1; }
fi
[ $fail -eq 0 ] && echo "audio tests: all passed" || echo "audio tests: FAILURES"
exit $fail
