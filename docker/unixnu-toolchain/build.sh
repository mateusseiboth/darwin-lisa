#!/bin/sh
# Build the toolchain image from a UniXNU checkout: build.sh [UNIXNU_DIR]
set -e
UNIXNU=${1:-$HOME/Dev/mobile/research/UniXNU}
HERE=$(cd "$(dirname "$0")" && pwd)
CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
cp "$HERE/Dockerfile" "$CTX/"
cp -r "$UNIXNU/scripts/linux" "$CTX/linux"
docker build -t darwin-lisa/unixnu-toolchain:$(git -C "$UNIXNU" rev-parse --short HEAD) -t darwin-lisa/unixnu-toolchain:latest "$CTX"
