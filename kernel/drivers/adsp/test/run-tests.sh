#!/bin/sh
# Host tests of dlisa.ssc.c's wire formats (no kernel, no phone): build with the host cc against shim/, run.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/ssc-test.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-format -fsanitize=undefined -fsanitize-trap=undefined -fstack-protector-all \
    -I"$HERE/shim" -o "$OUT/ssc-test" "$HERE/ssc-test.c"
"$OUT/ssc-test"
