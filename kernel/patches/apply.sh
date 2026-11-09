#!/bin/sh
# Apply the darwin-lisa EL0 emulation patches to a fetched UniXNU xnu-work tree.
# Usage: kernel/patches/apply.sh <path-to-xnu-work>
# Idempotent: skips files/hunks already present.
set -eu

tree=${1:?usage: apply.sh <xnu-work tree>}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$here/../pacstrip

test -f "$tree/osfmk/arm64/sleh.c" || { echo "not an xnu tree: $tree" >&2; exit 1; }

# 1. the emulator sources, compiled into osfmk (canonical copies live in kernel/pacstrip/)
for f in pac_emulate.c pac_emulate.h pac_trap.c pac_trap.h; do
	cp "$src/$f" "$tree/osfmk/arm64/$f"
done

# 2. the two edit patches (sleh.c call site + conf/files.arm64 listing)
for p in sleh-pac-emulate conf-files-pac; do
	if patch -d "$tree" -p1 --forward --reverse --dry-run <"$here/$p.patch" >/dev/null 2>&1; then
		echo "already applied: $p"
	else
		patch -d "$tree" -p1 --forward <"$here/$p.patch"
	fi
done

echo "darwin-lisa kernel emulation patches applied to $tree"
