#!/bin/sh
# darwin-lisa: host tests of the kernel patches' pure logic. Usage: kernel/patches/test/run.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# the helper block of sched-prefer-select.patch, as added lines, without the leading '+'
awk '/^\+TUNABLE\(uint32_t, lisa_sched_prefer_wait_us/ { on = 1 } on && !/^\+/ { exit } on { print substr($0, 2) }' \
	"$here/../sched-prefer-select.patch" >"$out/sched-prefer-select-block.h"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$out" -o "$out/t" "$here/sched-prefer-select-test.c"
"$out/t"
