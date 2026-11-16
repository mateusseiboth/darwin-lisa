#!/bin/sh
# Host tests of libdlisa-motion's pure parts (no phone, no toolchain container): sh userland/motion/test/run-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/dlisa-motion-tests.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
for t in "$HERE"/*-test.c; do
	name=$(basename "$t" .c)
	cc -Wall -Wextra -Werror -I"$HERE/.." -o "$OUT/$name" "$t" -lm
	printf '%s: ' "$name"
	"$OUT/$name"
done
