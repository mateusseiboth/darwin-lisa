#!/bin/sh
# Host test of `fw-adsp-stage.py header` (the ADSP firmware built into the kernel, adsp-wire.py step 5): the header is
# made twice from the staged firmware (the same bytes both times), then compiled with the host cc beside
# fw-builtin-test.c, which compares every built-in file with the staged one.
#   sh kernel/drivers/adsp/test/fw-builtin-test.sh [DIR]     (default research/hackios-xnu-kernel/fw-adsp)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
FW=${1:-/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/fw-adsp}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/adsp-fw-test.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
python3 -I "$HERE/../fw-adsp-stage.py" header "$FW" "$OUT/hakios.adsp_fw.h" >/dev/null
cp "$OUT/hakios.adsp_fw.h" "$OUT/first.h"
python3 -I "$HERE/../fw-adsp-stage.py" header "$FW" "$OUT/hakios.adsp_fw.h" >/dev/null
cmp -s "$OUT/first.h" "$OUT/hakios.adsp_fw.h" || { echo "fw-builtin-test: the header differs between two runs"; exit 1; }
cc -std=gnu11 -O1 -Wall -I"$OUT" -o "$OUT/fw-builtin-test" "$HERE/fw-builtin-test.c"
"$OUT/fw-builtin-test" "$FW"
