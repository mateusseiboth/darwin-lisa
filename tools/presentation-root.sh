#!/bin/sh
# darwin-lisa: the root image for the presentation build: the normal full image (FULL=1, every app) with
# userland/inject/presentation-springboard.patch in libdlisa.dylib (SpringBoard out of setup mode, the device
# activated, dlisa_unlock / dlisa_noidle / dlisa_watchdog). docs/presentation-build.md, step 2.
#
#   darwin-lisa/tools/presentation-root.sh          applies the patch (once), builds libdlisa into sysroot-26,
#                                                   rebuilds userland-26/out/system-26.apfs.full and checks its size
#
# Run it only when the data-volume worker is done with sysroot-26 and the root image: it writes both. It writes
# nothing to the phone. The image then goes to the phone as in docs/data-volume.md A5 (the root's 4608 MiB only).
set -eu
M=/home/mateusseiboth/Dev/mobile
D=$M/darwin-lisa
U=$M/userland-26
PATCH=userland/inject/presentation-springboard.patch
ROOT_BYTES=4831838208                   # 1179648 blocks: the data region starts right after (loader/data-regions.h)

cd "$D"
if git apply --reverse --check "$PATCH" 2>/dev/null; then
	echo "### $PATCH: already applied"
else
	git apply "$PATCH"
	echo "### $PATCH: applied"
fi
grep -q fixSpringBoard userland/inject/dlisa-inject.c
grep -q SpringBoard userland/inject/dlisa-inject.conf || { echo "dlisa-inject.conf does not name SpringBoard" >&2; exit 1; }

sh userland/inject/build-inject.sh
strings -a "$U/out/sysroot-26/usr/local/lib/libdlisa.dylib" | grep -q dlisa_unlock || { echo "libdlisa without the patch" >&2; exit 1; }

cd "$U"
# (userland-26's copy: the script finds out/ next to itself)
FULL=1 bash "$U/scripts/mkimage-trim.sh"
size=$(stat -c %s out/system-26.apfs.full)
if [ "$size" -ne "$ROOT_BYTES" ]; then
	echo "### system-26.apfs.full is $size bytes, not $ROOT_BYTES: the data region would have to move (docs/data-volume.md B1). Stop." >&2
	exit 1
fi
sha256sum out/system-26.apfs.full | tee out/system-26.apfs.full.sha256
echo "### root ready: write it with docs/data-volume.md A5 (count=4608), then var-write.sh verify"
