#!/bin/sh
# darwin-lisa: host tests of the display driver's pure parts (no phone, no toolchain container):
#   sh kernel/drivers/display/test/run-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
for t in "$HERE"/*_test.c; do
	name=$(basename "$t" .c)
	cc -std=c11 -Wall -Wextra -Werror -I"$HERE/.." -o "$OUT/$name" "$t"
	printf '%s:\n' "$name"
	"$OUT/$name"
done
