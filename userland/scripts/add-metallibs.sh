#!/usr/bin/env bash
# add-metallibs.sh [FS_DMG]: put every shader library (.metallib) of the decrypted root filesystem into out/sysroot-26.
#
# select-system-26.py left them all out (SKIP_EXT/SKIP_APP_EXT, 245 files, 452 MB plain, ~170 MB packed) while nothing
# but backboardd had a Metal device. With the plug-in in every process (boot-arg metal_all=1) each framework loads its
# own library the moment it has a device, and MetalPerformanceShaders ASSERTS when its sub-framework's default.metallib is
# missing (MPSDeviceComputePipelineState.mm:156, SpringBoard crash-looped through SiriUICore's blur, 2026-11-11 gpu-nc9).
#
# The list comes from extract/rootfs.slt (the 7z technical listing); 7z extracts into extract/metallibs/ and the files
# are then copied into the sysroot (staged applications, private/var/staged_system_apps/X, live in /Applications/X
# there, as select-system-26.py places them). Every copied file is checked against the listing's size: 7z inflates the
# decmpfs resource forks of this image, but a size mismatch would mean it did not, and the script stops.
# Space for them in the on-flash root (4,831,838,208 bytes) comes from the trees mkimage-trim.sh DROPS (device-only
# payload: the Apple ISP tables, the Apple GPU driver bundle, camera effects, face models, Health feed plug-ins, the
# onboarding bundles).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$(cd "$HERE/.." && pwd)"
EXTRACT="$BASE/extract"
SYSROOT="$BASE/out/sysroot-26"
FS_DMG="${1:-$EXTRACT/23F84__iPad17,1_2_3_4/094-56106-108.dmg}"
LIST="$EXTRACT/metallib.files"
STAGE="$EXTRACT/metallibs"
STAGED_APPS='private/var/staged_system_apps/'
# Which ones: select-system-26.py's rules (METALLIB, wanted_metallib: not the camera capture pipeline's nor the GPU
# capture replayer's, about 40 MB packed; not those of the bundles the image leaves out). Everything else,
# MetalPerformanceShaders whole (70 MB packed) first, comes in.

[[ -f $FS_DMG ]] || { echo "add-metallibs: no $FS_DMG (userland-26/scripts/build-system-26.sh extracts and decrypts it)" >&2; exit 1; }
[[ -f $EXTRACT/rootfs.slt ]] || { echo "add-metallibs: no $EXTRACT/rootfs.slt" >&2; exit 1; }

python3 -I - "$HERE/select-system-26.py" "$EXTRACT/rootfs.slt" "$LIST" "$STAGED_APPS" <<'EOF'
import importlib.util, stat, sys
rules, listing, out, staged = sys.argv[1:]
spec = importlib.util.spec_from_file_location('select_system_26', rules)
select = importlib.util.module_from_spec(spec)
spec.loader.exec_module(select)
paths = []
for entry in select.entries(listing):
    path, mode = entry['Path'], select.mode_bits(entry['Mode'])
    if mode is None or not stat.S_ISREG(mode) or not path.endswith(select.METALLIB):
        continue
    image = 'Applications/' + path[len(staged):] if path.startswith(staged) else path
    if image.startswith('private/var/') or not select.wanted_file(image, int(entry.get('Size') or 0)):
        continue
    paths.append((path, int(entry.get('Size') or 0)))
with open(out, 'w') as f:
    f.write(''.join(f'{p}\n' for p, _ in paths))
with open(out + '.sizes', 'w') as f:
    f.write(''.join(f'{s} {p}\n' for p, s in paths))
print(f'add-metallibs: {len(paths)} libraries, {sum(s for _, s in paths) / 1e6:.1f} MB plain')
EOF

# REUSE=1: the files extracted by an earlier run are still in extract/metallibs/
if [[ ${REUSE:-0} != 1 ]]; then
	rm -rf "$STAGE"; mkdir -p "$STAGE"
	7z x -y -o"$STAGE" "$FS_DMG" @"$LIST" > "$EXTRACT/extract-metallibs.log" 2>&1 || { echo "add-metallibs: 7z failed, see $EXTRACT/extract-metallibs.log" >&2; exit 1; }
fi

python3 -I - "$LIST.sizes" "$STAGE" "$SYSROOT" "$STAGED_APPS" <<'EOF'
import os, shutil, sys
sizes, stage, sysroot, staged = sys.argv[1:]
copied = total = 0
skipped = []
for line in open(sizes):
    size, _, path = line.rstrip('\n').partition(' ')
    src = os.path.join(stage, path)
    if not os.path.isfile(src):
        sys.exit(f'add-metallibs: 7z did not extract {path}')
    got = os.path.getsize(src)
    if got != int(size):
        sys.exit(f'add-metallibs: {path} is {got} bytes, the listing says {size}: not inflated')
    dest = 'Applications/' + path[len(staged):] if path.startswith(staged) else path
    target = os.path.join(sysroot, dest)
    if not os.path.isdir(os.path.dirname(target)):
        skipped.append(dest)        # its bundle is not in the sysroot: nothing there would load it
        continue
    if os.path.islink(target):      # QuartzCore's: a link into the OS cryptex, whose copy is in the sysroot already
        continue
    try:
        shutil.copyfile(src, target)
    except OSError as e:
        sys.exit(f'add-metallibs: cannot write {target}: {e}')
    os.chmod(target, 0o644)
    copied += 1; total += got
print(f'add-metallibs: {copied} libraries in the sysroot, {total / 1e6:.1f} MB plain; {len(skipped)} without a bundle there')
EOF
