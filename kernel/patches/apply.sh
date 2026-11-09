#!/bin/sh
# Apply the darwin-lisa EL0 emulation patches to a fetched UniXNU xnu-work tree.
# Usage: kernel/patches/apply.sh <path-to-xnu-work>
# Idempotent: skips files/hunks already present.
set -eu

tree=${1:?usage: apply.sh <xnu-work tree>}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$here/../pacstrip

test -f "$tree/osfmk/arm64/sleh.c" || { echo "not an xnu tree: $tree" >&2; exit 1; }

# 1. the emulator + rewriter sources, compiled into osfmk (canonical copies in kernel/pacstrip/)
for f in pac_rewrite.c pac_rewrite.h pac_emulate.c pac_emulate.h pac_trap.c pac_trap.h; do
	cp "$src/$f" "$tree/osfmk/arm64/$f"
done

# 2. the edit patches: sleh.c EL0 hook, conf/files.arm64 listing, the loader
#    accepting the arm64e slice, and the PAC-stripping pager in the loader.
for p in sleh-pac-emulate conf-files-pac machdep-grade-arm64e mach_loader-pac-pager; do
	if patch -d "$tree" -p1 --forward --reverse --dry-run <"$here/$p.patch" >/dev/null 2>&1; then
		echo "already applied: $p"
	else
		patch -d "$tree" -p1 --forward <"$here/$p.patch"
	fi
done

echo "darwin-lisa kernel emulation patches applied to $tree"
