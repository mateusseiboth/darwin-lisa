#!/bin/sh
# darwin-lisa: host tests of the kernel patches' pure logic. Usage: kernel/patches/test/run.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# block PATCH FIRST-LINE: a patch's helper block, as added lines from the first one that starts with FIRST-LINE to the
# first line that is not an addition, without the leading '+'
block() {
	awk -v start="$2" 'index($0, start) == 1 { on = 1 } on && !/^\+/ { exit } on { print substr($0, 2) }' "$here/../$1.patch"
}
block sched-prefer-select '+TUNABLE(uint32_t, lisa_sched_prefer_wait_us' >"$out/sched-prefer-select-block.h"
block sched-idle-quiet '+TUNABLE(uint64_t, lisa_idle_wfi_mask' >"$out/sched-idle-quiet-block.h"
for t in sched-prefer-select sched-idle-quiet; do
	echo "== $t"
	cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$out" -o "$out/$t" "$here/$t-test.c"
	"$out/$t"
done
