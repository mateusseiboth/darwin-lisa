#!/bin/sh
# The camera driver's host tests: the frame's log summary and exposure step (C, camera_snap.h), the sensor tables
# against pmOS's Linux driver (camtab.py), the driver's register numbers against Linux and pmOS's device tree, and the
# log-to-picture tool. Nothing here needs the phone or docker.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/dlisa-camera-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
fail=0
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/camera_snap_test" "$HERE/camera_snap_test.c" && "$OUT/camera_snap_test" || fail=1
for t in "$HERE"/test_*.py; do
	python3 -I "$t" -q 2>&1 | tail -n 3 || fail=1
	python3 -I "$t" -q >/dev/null 2>&1 || { echo "FAIL: $t"; fail=1; }
done
[ $fail -eq 0 ] && echo "camera tests: all passed" || echo "camera tests: FAILURES"
exit $fail
