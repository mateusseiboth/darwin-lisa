#!/bin/sh
# Host tests of the sensors' FastRPC file server (no kernel, no phone):
#   1. dlisa.fastrpc_proto.h's wire formats and file server (fastrpc-test.c): gcc with UBSan traps, then clang with
#      ASan+UBSan when the host has the runtime, then valgrind when installed;
#   2. fw-sensors-stage.py: a made-up tree staged, packed into the archive the kernel takes, and read back through the
#      same server (sensors-stage-test.sh + sensors-archive-test.c).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/fastrpc-test.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror"
cc $FLAGS -fsanitize=undefined -fsanitize-trap=undefined -fstack-protector-all -o "$OUT/fastrpc-test" "$HERE/fastrpc-test.c"
"$OUT/fastrpc-test"
if command -v clang > /dev/null && clang $FLAGS -fsanitize=address,undefined -o "$OUT/fastrpc-test-asan" \
    "$HERE/fastrpc-test.c" 2> /dev/null; then
	echo "asan:"; "$OUT/fastrpc-test-asan"
fi
if command -v valgrind > /dev/null; then
	echo "valgrind:"; valgrind -q --leak-check=full --error-exitcode=9 "$OUT/fastrpc-test"
fi
cc $FLAGS -fsanitize=undefined -fsanitize-trap=undefined -o "$OUT/sensors-archive-test" "$HERE/sensors-archive-test.c"
sh "$HERE/sensors-stage-test.sh" "$OUT/sensors-archive-test"
