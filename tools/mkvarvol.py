#!/usr/bin/env python3
"""Make the data volume (/private/var) for the phone: an empty, journaled, case-sensitive HFS Plus (HFSX) volume of
exactly the region's size, in blocks of 4096, its root owned by root:wheel 0755 as on an iPhone.

usage: mkvarvol.py OUT.img [--blocks N] [--mkfs PATH] [--name Data]

    --blocks   the region's size in blocks of 4096 (default 1048576: option A, super's tail, loader/data-regions.h)

Writes OUT.img (sparse, exactly N*4096 bytes), OUT.img.gz (what goes to pmOS: a few MB) and OUT.img.sha256 (of the
raw image, what tools/var-write.sh checks before and after it writes). docs/data-volume.md has the device steps.

Why empty: Linux cannot write a journaled HFS Plus volume, and pre-populating it would mean a non-journaled volume,
root on this computer and a loop mount. init26 lays down the same skeleton (var-skel.h, the logd markers,
/private/var/keybags) with the same owners and modes on the first boot that finds it missing, and leaves it alone
on every boot after (userland/init/init-26.c, mountVar). The kernel's HFS (bsd/hfs) replays the journal at mount.

Why HFS Plus and not APFS: the kernel's APFS (bsd/miscfs/apfs, HakiOS's own) mounts read only; it is not Apple's
apfs kext. HakiOS used HFSX on /dev/ufs_darwin_data the same way.
"""
import argparse
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys

BS = 4096
A_BLOCKS = 1048576                      # super blocks [1179648, 2228224): loader/data-regions.h
HFSX_SIG = 0x4858                       # 'HX': case-sensitive HFS Plus
JOURNALED_BIT = 1 << 13                 # kHFSVolumeJournaledBit in the volume header's attributes
CHUNK = 16 << 20


def check_header(path, blocks):
    """The volume header at 1024: HFSX, journaled, blocks of 4096, exactly `blocks` of them; and its copy at the end."""
    with open(path, 'rb') as f:
        f.seek(1024)
        vh = f.read(512)
        f.seek(blocks * BS - 1024)
        alt = f.read(512)
    sig, version, attributes = struct.unpack('>HHI', vh[0:8])
    block_size, total = struct.unpack('>II', vh[40:48])
    why = None
    if sig != HFSX_SIG:
        why = f'signature {sig:#x}, not HX (case-sensitive HFS Plus)'
    elif not attributes & JOURNALED_BIT:
        why = 'not journaled'
    elif block_size != BS or total != blocks:
        why = f'{total} blocks of {block_size}, not {blocks} of {BS}'
    elif alt[0:2] != vh[0:2]:
        why = 'no alternate volume header at the end'
    if why:
        sys.exit(f'mkvarvol.py: {path}: {why}')
    print(f'{path}: HFSX, journaled, {total} blocks of {block_size} ({total * block_size >> 20} MB)')


def sha256_and_gzip(path):
    """sha256 of the raw image, and OUT.img.gz alongside, in one pass."""
    h = hashlib.sha256()
    with open(path, 'rb') as src, gzip.open(path + '.gz', 'wb', compresslevel=1) as gz:
        while chunk := src.read(CHUNK):
            h.update(chunk)
            gz.write(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--blocks', type=int, default=A_BLOCKS)
    ap.add_argument('--mkfs', default=shutil.which('mkfs.hfsplus') or 'mkfs.hfsplus')
    ap.add_argument('--name', default='Data')
    a = ap.parse_args()
    if a.blocks < 32768:
        sys.exit('mkvarvol.py: fewer than 32768 blocks (128 MB) is not a data volume')
    if os.path.exists(a.out):
        os.unlink(a.out)
    with open(a.out, 'wb') as f:
        f.truncate(a.blocks * BS)
    # root's, 0755, as /private/var on an iPhone; -s case-sensitive, -J journaled (default journal size)
    subprocess.run([a.mkfs, '-s', '-J', '-b', str(BS), '-U', '0', '-G', '0', '-M', '755', '-v', a.name, a.out],
                   check=True)
    if os.path.getsize(a.out) != a.blocks * BS:
        sys.exit('mkvarvol.py: mkfs changed the image size')
    check_header(a.out, a.blocks)
    fsck = shutil.which('fsck.hfsplus')
    if fsck:
        # -fy, not -fn: hfsprogs' fsck_hfs 540.1 reports every new journaled volume as "Volume header needs minor
        # repair (1, 0)" with -n ("invalid primary VHB ... result 35"), and with -y finds it OK and changes not one
        # byte (checked with cmp on a 1 GB volume made the same way). The header is checked again after it.
        r = subprocess.run([fsck, '-fy', a.out], capture_output=True, text=True)
        last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
        print(f'fsck.hfsplus -fy: exit {r.returncode}: {last}')
        if r.returncode != 0 or 'appears to be OK' not in last:
            sys.exit('mkvarvol.py: fsck.hfsplus does not find the new volume clean')
        check_header(a.out, a.blocks)
    digest = sha256_and_gzip(a.out)
    with open(a.out + '.sha256', 'w') as f:
        f.write(f'{digest}  {os.path.basename(a.out)}\n')
    print(f'{a.out}: sha256 {digest}; {a.out}.gz: {os.path.getsize(a.out + ".gz")} bytes')


if __name__ == '__main__':
    main()
