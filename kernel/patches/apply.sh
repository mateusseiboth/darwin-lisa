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
cp "$here/../soc/gic_vtimer.c" "$tree/osfmk/arm64/gic_vtimer.c"

# 2. the edit patches, in this order: sleh.c EL0 hook, conf/files.arm64 listing, the loader accepting the arm64e
#    slice, the PAC-stripping pager, the loader console, ..., and last the boot framebuffer mapped write-combined on
#    BCM2837 too, as HakiOS (kernel/drivers/gpu/fb-wcomb.patch: as Device memory MiniIOMobileFramebuffer took ~78 ms a
#    frame and DC ZVA (bzero) faulted on the panel).
series="sleh-pac-emulate conf-files-pac machdep-grade-arm64e mach_loader-pac-pager
	pe_kprintf-loader-console pe_gen-loader-console debug-loader-return
	prng-seed-bringup pe_init-skip-console machine-routines-virtual-timer machine-routines-decrementer-floor
	serial-console-tee vcputc-tee bcm2837-gic-v3 pe_fiq-vtimer-group1 trustcache-skip cpu-idle-no-wfi
	boot-args-loader-layout cpu-subtype-arm64e hfs-rename-swap sched-prefer-cores sched-prefer-ui
	sched-prefer-select sched-idle-quiet ../drivers/gpu/fb-wcomb"

# The series is a stack: a later patch may change lines an earlier one added (sched-idle-quiet edits
# sched-prefer-select's), and then the earlier one's reverse no longer matches the tree, so "reverse applies = already
# applied" tested patch by patch called it missing and put 4 of its 13 hunks in a second time (2026-11-16). Which
# ones the tree has is found on a scratch copy of the files the series touches instead: the patches are taken off it
# from the last to the first, and one that comes off cleanly there is applied in the tree. Nothing is written to the
# tree before every missing patch is known to apply cleanly.
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
# the scratch copy: the tree's own version of every file the series touches
copy_touched() {
	rm -rf "$scratch"/*
	for p in $series; do
		awk '/^\+\+\+ / { sub(/^b\//, "", $2); print $2 }' "$here/$p.patch"
	done | sort -u | while read -r f; do
		if [ -f "$tree/$f" ]; then
			mkdir -p "$scratch/$(dirname "$f")"
			cp "$tree/$f" "$scratch/$f"
		fi
	done
}
copy_touched
applied=" "
for p in $(printf '%s\n' $series | awk '{ line[NR] = $0 } END { for (i = NR; i > 0; i--) print line[i] }'); do
	if patch -d "$scratch" -p1 --reverse --force --dry-run <"$here/$p.patch" >/dev/null 2>&1; then
		patch -d "$scratch" -p1 --reverse --force --no-backup-if-mismatch <"$here/$p.patch" >/dev/null 2>&1
		applied="$applied$p "
	fi
done
missing=
for p in $series; do
	case "$applied" in
	*" $p "*) echo "already applied: ${p##*/}" ;;
	*) missing="$missing $p" ;;
	esac
done
# the missing ones in order, each checked on a fresh scratch copy (with the ones before it) first: all or nothing
copy_touched
for p in $missing; do
	if ! patch -d "$scratch" -p1 --forward --force --no-backup-if-mismatch <"$here/$p.patch" >/dev/null 2>&1; then
		echo "apply.sh: ${p##*/} does not apply to $tree; nothing written:" >&2
		patch -d "$tree" -p1 --forward --dry-run <"$here/$p.patch" >&2 || true
		exit 1
	fi
done
for p in $missing; do
	patch -d "$tree" -p1 --forward --no-backup-if-mismatch <"$here/$p.patch"
done

# 4. the cameras (kernel/drivers/camera/camera-wire.py: option HAKIOS_CAMERA, inert without cam=N). Its sensor table is
#    made from pmOS's sensor driver patch; without that patch on the machine the wire says why and the build goes on
#    without the camera.
XNU_WORK="$tree" python3 "$here/../drivers/camera/camera-wire.py" || echo "apply.sh: camera wire NOT applied (see above)"

echo "darwin-lisa kernel emulation patches applied to $tree"
