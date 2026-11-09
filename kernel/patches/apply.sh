#!/bin/sh
# Apply the darwin-lisa EL0 emulation patches to a fetched UniXNU xnu-work tree.
# Usage: kernel/patches/apply.sh <path-to-xnu-work>
# Idempotent: skips files/hunks already present.
set -eu

tree=${1:?usage: apply.sh <xnu-work tree>}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$here/../pacstrip

test -f "$tree/osfmk/arm64/sleh.c" || { echo "not an xnu tree: $tree" >&2; exit 1; }

# 1. the emulator + rewriter + loader-console sources, compiled into osfmk
#    (canonical copies in kernel/pacstrip/ and kernel/loader-console/)
for f in pac_rewrite.c pac_rewrite.h pac_emulate.c pac_emulate.h pac_trap.c pac_trap.h; do
	cp "$src/$f" "$tree/osfmk/arm64/$f"
done
for f in loader_console.c loader_console.h; do
	cp "$here/../loader-console/$f" "$tree/osfmk/arm64/$f"
done

# 2. the edit patches: sleh.c EL0 hook, conf/files.arm64 listing, the loader
#    accepting the arm64e slice, the PAC-stripping pager, and the loader console.
for p in sleh-pac-emulate conf-files-pac machdep-grade-arm64e mach_loader-pac-pager \
         pe_kprintf-loader-console pe_gen-loader-console debug-loader-return \
         prng-seed-bringup pe_init-skip-console machine-routines-virtual-timer; do
	if patch -d "$tree" -p1 --forward --reverse --dry-run <"$here/$p.patch" >/dev/null 2>&1; then
		echo "already applied: $p"
	else
		patch -d "$tree" -p1 --forward <"$here/$p.patch"
	fi
done

echo "darwin-lisa kernel emulation patches applied to $tree"
