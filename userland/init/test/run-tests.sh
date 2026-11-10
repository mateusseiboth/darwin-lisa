#!/bin/sh
# Host tests of init26's pure parts (no phone, no toolchain container): sh userland/init/test/run-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/init26-tests.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
for t in "$HERE"/*-test.c; do
	name=$(basename "$t" .c)
	cc -Wall -Wextra -Werror -I"$HERE/.." -o "$OUT/$name" "$t"
	printf '%s: ' "$name"
	"$OUT/$name"
done
