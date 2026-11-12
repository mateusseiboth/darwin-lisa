#!/usr/bin/env python3
"""prune-sysroot.py [--apply | --restore [FS_DMG]] [SYSROOT]: remove from an existing out/sysroot-26 the files that
select-system-26.py no longer chooses, so that a change to its rules need not start again from the decrypted root
filesystem; or put them back.

Only files that came from Apple's image are candidates: a path is deleted when it is in extract/rootfs.slt (the
listing select-system-26.py reads; staged applications under their /Applications name) AND wanted_file() now says no.
What this port added (init26, libdlisa, HakiosMetal.bundle, the event timing profiles...) is not in that listing and is
never touched. Without --apply it only prints what would go and how much it weighs. --apply also writes what it
removed to extract/pruned.files (Apple's paths), and --restore extracts those again from the decrypted FileSystem dmg
(7z) into the sysroot: the reversible step. The rules themselves are switched with APPLE_ONLY=keep (select-system-26.py).

2026-11-11: made for the shader libraries. select-system-26.py dropped the Neural Engine models (.hwx), the ISP and
display tables, the Apple GPU driver bundle and the camera effects' pictures (326 MB) to make room in the on-flash root
for the .metallib files add-metallibs.sh brings in. `APPLE_ONLY=keep python3 prune-sysroot.py --restore` undoes it.
"""
import importlib.util
import os
import shutil
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXTRACT = os.path.normpath(os.path.join(HERE, '..', 'extract'))
STAGED_APPS = 'private/var/staged_system_apps/'
PRUNED = os.path.join(EXTRACT, 'pruned.files')
DEFAULT_DMG = os.path.join(EXTRACT, '23F84__iPad17,1_2_3_4', '094-56106-108.dmg')


def loadSelect():
    spec = importlib.util.spec_from_file_location('select_system_26', os.path.join(HERE, 'select-system-26.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def imagePath(path):
    return 'Applications/' + path[len(STAGED_APPS):] if path.startswith(STAGED_APPS) else path


def appleFiles(select, listing):
    """path in our image -> size, for every regular file of Apple's image"""
    files = {}
    for entry in select.entries(listing):
        mode = select.mode_bits(entry['Mode'])
        if mode is None or not stat.S_ISREG(mode) or entry['Path'].startswith('private/var/') and not entry['Path'].startswith(STAGED_APPS):
            continue
        files[imagePath(entry['Path'])] = int(entry.get('Size') or 0)
    return files


def unwanted(select, sysroot, files):
    for path, size in files.items():
        full = os.path.join(sysroot, path)
        if not os.path.isfile(full) or os.path.islink(full):
            continue
        if select.wanted_file(path, size):
            continue
        yield path, os.path.getsize(full)


def applePath(imagePath):
    return STAGED_APPS + imagePath[len('Applications/'):] if imagePath.startswith('Applications/') else imagePath


def savePruned(paths):
    """Apple's paths of what went, added to extract/pruned.files (one line each, kept unique)"""
    known = set(open(PRUNED).read().split('\n')) if os.path.exists(PRUNED) else set()
    with open(PRUNED, 'a') as f:
        f.write(''.join(f'{path}\n' for path in paths if path not in known))


def restore(sysroot, dmg):
    """every path of extract/pruned.files back into the sysroot, from the decrypted FileSystem dmg"""
    paths = [line for line in open(PRUNED).read().split('\n') if line]
    stage = os.path.join(EXTRACT, 'pruned-restore')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    listing = os.path.join(stage, 'restore.files')
    with open(listing, 'w') as f:
        f.write(''.join(f'{path}\n' for path in paths))
    subprocess.run(['7z', 'x', '-y', f'-o{stage}', dmg, f'@{listing}'], check=True, stdout=subprocess.DEVNULL)
    restored = 0
    for path in paths:
        source = os.path.join(stage, path)
        if not os.path.isfile(source):
            sys.exit(f'prune-sysroot: 7z did not extract {path}')
        target = os.path.join(sysroot, imagePath(path))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(source, target)
        os.chmod(target, 0o644)
        restored += 1
    shutil.rmtree(stage, ignore_errors=True)
    print(f'prune-sysroot: {restored} files restored from {dmg}')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    apply = '--apply' in sys.argv
    if '--restore' in sys.argv:
        dmg = args[0] if args and args[0].endswith('.dmg') else DEFAULT_DMG
        rest = args[1:] if args and args[0].endswith('.dmg') else args
        restore(rest[0] if rest else os.path.join(HERE, '..', 'out', 'sysroot-26'), dmg)
        return
    sysroot = args[0] if args else os.path.join(HERE, '..', 'out', 'sysroot-26')
    listing = os.path.join(EXTRACT, 'rootfs.slt')
    select = loadSelect()
    gone = list(unwanted(select, sysroot, appleFiles(select, listing)))
    byTop = {}
    for path, size in gone:
        top = '/'.join(path.split('/')[:3])
        byTop[top] = byTop.get(top, 0) + size
    for top, size in sorted(byTop.items(), key=lambda item: -item[1])[:25]:
        print(f'{size / 2**20:8.1f} MB  {top}')
    print(f'prune-sysroot: {len(gone)} files, {sum(size for _, size in gone) / 2**20:.1f} MB' + ('' if apply else ' (dry run: --apply removes them)'))
    if not apply:
        return
    savePruned(applePath(path) for path, _ in gone)
    for path, _ in gone:
        os.unlink(os.path.join(sysroot, path))
    for path, _ in gone:
        folder = os.path.dirname(os.path.join(sysroot, path))
        while folder != sysroot and os.path.isdir(folder) and not os.listdir(folder):
            os.rmdir(folder)
            folder = os.path.dirname(folder)


if __name__ == '__main__':
    main()
