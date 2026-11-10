#!/usr/bin/env python3
"""Write the iOS root (an APFS container image) into ONE named GPT partition of lisa's UFS LUN 0, and nowhere else.

Runs where LUN 0 is a local block device: on the PC with the phone in U-Boot's USB disk mode (tools/bootos.sh ums,
the disk shows up as /dev/sdX, vendor "Linux", model "File-Store Gadget"), or on pmOS itself (/dev/sda), as root.
Adapted from HakiOS's tools/darwin-write.py (adb only) with the same refusals.

usage:
  darwin-write.py check  --disk DEV [--part super]          the GPT and the partition's identity        (writes nothing)
  darwin-write.py sum    --disk DEV [--part super] BLOCK COUNT  sha256 of COUNT 4 KiB blocks from BLOCK   (writes nothing)
  darwin-write.py plan   --disk DEV [--part super] IMAGE      every check `write` makes                   (writes nothing)
  darwin-write.py write  --disk DEV [--part super] IMAGE      with DARWIN_WRITE=yes: IMAGE at block 0, read back, compared
  darwin-write.py selftest                                     the GPT checks against the saved 2026-11-08 GPT (writes nothing)

What it takes to write (all checked right before the write, from the device itself):
  - DEV is a whole disk with 4096-byte logical blocks whose GPT is lisa's LUN 0: header and table CRCs right in the
    primary and the backup copy, the two tables identical, the backup at the disk's last block, the disk GUID and the
    last usable LBA the ones saved from the phone (EXPECT below, or --expect FILE written by `check --save FILE`).
  - the partition is the one entry with that name, and its number, type GUID, unique GUID and LBA range are the
    expected ones; no other entry overlaps it; the kernel's partition node (DEVpN or DEVN) has the same start and
    size in sysfs; nothing holds it (no mount, no device-mapper, no loop).
  - what is in the partition now is Android's LP metadata, an APFS container, or zeros: never a partition table
    (pmOS's image has one) and never an ext2/3/4 file system.
  - IMAGE is an APFS container (NXSB, 4096-byte blocks), a whole number of blocks, and fits.
  - only the partition node is opened for writing, never the whole disk. Afterwards the written range is dropped from
    the page cache (BLKFLSBUF), read back and its sha256 compared with IMAGE's.
Every attempt, refused or done, goes into darwin-lisa/logs/darwin-write.log.
"""
import argparse
import fcntl
import hashlib
import json
import os
import stat
import struct
import sys
import time
import uuid
import zlib

BS = 4096
CHUNK = 8 << 20
BLKFLSBUF = 0x1261
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'logs', 'darwin-write.log')
BACKUP = os.path.join(os.path.dirname(ROOT), 'backups', 'lisa-pmos-20261108')

# LUN 0 of this phone as the GPT backup of 2026-11-08 has it (backups/lisa-pmos-20261108/gpt-sda-head.bin)
EXPECT = {
    'disk_guid': 'd363770d-93d1-138e-73ef-5c047f0d33ed',
    'last_usable': 30617594,
    'parts': {
        'super': {'number': 20, 'first': 573440, 'last': 2801663,
                  'type': '89a12de1-5e41-4cb3-8b4c-b1441eb5da38', 'unique': 'f1cc2373-2d04-a1a1-5cbf-e3d75f3a4a27'},
    },
}


class Refused(Exception):
    pass


def log(line):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), line))
    print('darwin-write: ' + line)


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def header(block):
    if block[:8] != b'EFI PART':
        raise Refused('no GPT header where one should be')
    size = struct.unpack_from('<I', block, 12)[0]
    h = bytearray(block[:size])
    want = struct.unpack_from('<I', h, 16)[0]
    struct.pack_into('<I', h, 16, 0)
    my, alt, first_usable, last_usable = struct.unpack_from('<QQQQ', block, 24)
    return {'crc_ok': crc(bytes(h)) == want, 'my': my, 'alternate': alt, 'first_usable': first_usable,
            'last_usable': last_usable, 'disk_guid': str(uuid.UUID(bytes_le=bytes(block[56:72]))),
            'entries_lba': struct.unpack_from('<Q', block, 72)[0], 'count': struct.unpack_from('<I', block, 80)[0],
            'entry_size': struct.unpack_from('<I', block, 84)[0], 'entries_crc': struct.unpack_from('<I', block, 88)[0]}


def entries(raw, h):
    out = []
    for i in range(h['count']):
        e = raw[i * h['entry_size']:(i + 1) * h['entry_size']]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', e, 32)
        out.append({'number': i + 1, 'name': e[56:128].decode('utf-16le').split('\0', 1)[0],
                    'type': str(uuid.UUID(bytes_le=bytes(e[:16]))), 'unique': str(uuid.UUID(bytes_le=bytes(e[16:32]))),
                    'first': first, 'last': last})
    return out


def read_gpt(read, nblocks):
    """read(lba, count) -> bytes. Both copies checked; returns (primary header, parsed entries)."""
    ph = header(read(1, 1))
    table_blocks = (ph['count'] * ph['entry_size'] + BS - 1) // BS
    praw = read(ph['entries_lba'], table_blocks)[:ph['count'] * ph['entry_size']]
    bh = header(read(nblocks - 1, 1)) if nblocks else header(read(ph['alternate'], 1))
    braw = read(bh['entries_lba'], table_blocks)[:bh['count'] * bh['entry_size']]
    bad = []
    if not ph['crc_ok'] or crc(praw) != ph['entries_crc']:
        bad.append('primary GPT CRC wrong')
    if not bh['crc_ok'] or crc(braw) != bh['entries_crc']:
        bad.append('backup GPT CRC wrong')
    if praw != braw:
        bad.append('primary and backup tables differ')
    if ph['my'] != 1 or (nblocks and (ph['alternate'] != nblocks - 1 or bh['my'] != nblocks - 1)):
        bad.append('GPT headers are not at LBA 1 and at the last block')
    if bad:
        raise Refused('; '.join(bad))
    return ph, entries(praw, ph)


def check_identity(ph, ents, name, expect):
    want = expect['parts'].get(name)
    if want is None:
        raise Refused(f'no expected identity for {name!r} (pass --expect FILE from `check --save`)')
    if ph['disk_guid'] != expect['disk_guid'] or ph['last_usable'] != expect['last_usable']:
        raise Refused(f"this is not lisa's LUN 0 as saved: disk GUID {ph['disk_guid']}, last usable {ph['last_usable']}")
    found = [e for e in ents if e['name'] == name]
    if len(found) != 1:
        raise Refused(f'{len(found)} entries named {name}')
    p = found[0]
    for k in ('number', 'first', 'last', 'type', 'unique'):
        if p[k] != want[k]:
            raise Refused(f'{name}: {k} is {p[k]}, expected {want[k]}')
    for e in ents:
        if e is not p and e['first'] <= p['last'] and e['last'] >= p['first']:
            raise Refused(f"entry {e['number']} ({e['name']}) overlaps {name}")
    return p


class Disk:
    def __init__(self, path):
        self.path = os.path.realpath(path)
        st = os.stat(self.path)
        if not stat.S_ISBLK(st.st_mode):
            raise Refused(f'{path} is not a block device')
        self.name = os.path.basename(self.path)
        sysd = f'/sys/block/{self.name}'
        if not os.path.isdir(sysd):
            raise Refused(f'{path} is not a whole disk (give /dev/sdX, not a partition)')
        self.sys = sysd
        lbs = int(open(f'{sysd}/queue/logical_block_size').read())
        if lbs != BS:
            raise Refused(f'{path} has {lbs}-byte logical blocks; lisa LUN 0 has 4096')
        self.blocks = int(open(f'{sysd}/size').read()) * 512 // BS
        self.model = ' '.join(open(f'{sysd}/device/{k}').read().strip() for k in ('vendor', 'model')
                              if os.path.exists(f'{sysd}/device/{k}'))
        self.fd = os.open(self.path, os.O_RDONLY)

    def read(self, lba, count):
        return os.pread(self.fd, count * BS, lba * BS)

    def part_node(self, number):
        for cand in (f'{self.name}{number}', f'{self.name}p{number}'):
            if os.path.isdir(f'{self.sys}/{cand}'):
                return cand
        raise Refused(f'no kernel node for partition {number} of {self.name} (partprobe?)')


def check_node(disk, p):
    node = disk.part_node(p['number'])
    d = f'{disk.sys}/{node}'
    start = int(open(f'{d}/start').read()) * 512 // BS
    size = int(open(f'{d}/size').read()) * 512 // BS
    if (start, size) != (p['first'], p['last'] - p['first'] + 1):
        raise Refused(f'/dev/{node} is blocks {start}+{size} in sysfs, the GPT says {p["first"]}..{p["last"]}')
    if os.listdir(f'{d}/holders'):
        raise Refused(f'/dev/{node} is held by {os.listdir(f"{d}/holders")}')
    for line in open('/proc/mounts'):
        if os.path.realpath(line.split()[0]) == f'/dev/{node}':
            raise Refused(f'/dev/{node} is mounted')
    return f'/dev/{node}'


def check_content(disk, p):
    head = disk.read(p['first'], 4)
    if head[BS:BS + 8] == b'EFI PART' or head[512:520] == b'EFI PART':
        raise Refused('the partition holds a partition table (a pmOS image?): not writing over it')
    if struct.unpack_from('<H', head, 1024 + 56)[0] == 0xEF53:
        raise Refused('the partition holds an ext2/3/4 file system: not writing over it')
    if struct.unpack_from('<I', head, 4096)[0] == 0x616C4467:
        return 'Android LP metadata (super)'
    if head[32:36] == b'NXSB':
        return 'an APFS container'
    if head == bytes(len(head)):
        return 'zeros'
    raise Refused('the partition holds something unknown (not LP metadata, APFS or zeros): check it by hand')


def check_image(path, p):
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        b0 = f.read(BS)
    if b0[32:36] != b'NXSB' or struct.unpack_from('<I', b0, 36)[0] != BS:
        raise Refused(f'{path} is not an APFS container with 4096-byte blocks')
    if size % BS:
        raise Refused(f'{path} is not a whole number of 4096-byte blocks')
    room = p['last'] - p['first'] + 1
    if size // BS > room:
        raise Refused(f'{path} is {size // BS} blocks, the partition {room}')
    return size


def sha256_of(read_chunk, total):
    h, done = hashlib.sha256(), 0
    while done < total:
        n = min(CHUNK, total - done)
        h.update(read_chunk(done, n))
        done += n
    return h.hexdigest()


def load_expect(path):
    if not path:
        return EXPECT
    return json.load(open(path))


def prepare(a):
    disk = Disk(a.disk)
    ph, ents = read_gpt(disk.read, disk.blocks)
    p = check_identity(ph, ents, a.part, load_expect(a.expect))
    node = check_node(disk, p)
    return disk, ph, ents, p, node


def cmd_check(a):
    disk = Disk(a.disk)
    ph, ents = read_gpt(disk.read, disk.blocks)
    print(f'{disk.path}: {disk.blocks} blocks of {BS}, "{disk.model}", disk GUID {ph["disk_guid"]}, '
          f'last usable {ph["last_usable"]}, GPT good (both copies)')
    for e in ents:
        print(f"  {e['number']:3d} {e['name']:20s} {e['first']:10d}..{e['last']:10d} "
              f"{(e['last'] - e['first'] + 1) * BS / 2**30:8.2f} GiB  {e['type']}  {e['unique']}")
    if a.save:
        json.dump({'disk_guid': ph['disk_guid'], 'last_usable': ph['last_usable'],
                   'parts': {e['name']: {k: e[k] for k in ('number', 'first', 'last', 'type', 'unique')}
                             for e in ents}}, open(a.save, 'w'), indent=1)
        print(f'identity saved to {a.save} (pass it as --expect to plan/write)')
    p = check_identity(ph, ents, a.part, load_expect(a.expect))
    node = check_node(disk, p)
    print(f'{a.part}: entry {p["number"]}, {node}, blocks {p["first"]}..{p["last"]}: identity as expected; '
          f'it holds {check_content(disk, p)}')


def cmd_sum(a):
    disk, _, _, p, _ = prepare(a)
    if a.block < 0 or a.count <= 0 or a.block + a.count > p['last'] - p['first'] + 1:
        raise Refused('blocks outside the partition')
    base = (p['first'] + a.block) * BS
    print(sha256_of(lambda off, n: os.pread(disk.fd, n, base + off), a.count * BS))


def cmd_plan(a, write=False):
    disk, _, _, p, node = prepare(a)
    content = check_content(disk, p)
    size = check_image(a.image, p)
    img_fd = os.open(a.image, os.O_RDONLY)
    img_sha = sha256_of(lambda off, n: os.pread(img_fd, n, off), size)
    log(f'plan: {a.image} ({size} bytes, {size // BS} blocks, sha256 {img_sha}) -> {node} = {a.part} '
        f'blocks {p["first"]}..{p["first"] + size // BS - 1} of {disk.path} ("{disk.model}"); it holds {content} now')
    if not write:
        print('nothing written (plan). DARWIN_WRITE=yes darwin-write.py write ... to do it.')
        return
    if os.environ.get('DARWIN_WRITE') != 'yes':
        raise Refused('DARWIN_WRITE=yes is not set')
    # the node again, right before opening it for writing
    if check_node(disk, p) != node:
        raise Refused('the partition node changed')
    out = os.open(node, os.O_WRONLY)
    try:
        if os.lseek(out, 0, os.SEEK_END) != (p['last'] - p['first'] + 1) * BS:
            raise Refused(f'{node} is not the size of {a.part}')
        done, t0 = 0, time.time()
        while done < size:
            n = min(CHUNK, size - done)
            buf = os.pread(img_fd, n, done)
            if os.pwrite(out, buf, done) != n:
                raise Refused(f'short write at {done}')
            done += n
            if done % (256 << 20) == 0 or done == size:
                print(f'\r  {done >> 20} / {size >> 20} MiB  {done / (time.time() - t0 + 1e-9) / 2**20:.1f} MiB/s',
                      end='', flush=True)
        print()
        os.fsync(out)
        fcntl.ioctl(out, BLKFLSBUF, 0)
    finally:
        os.close(out)
    rd = os.open(node, os.O_RDONLY)
    try:
        fcntl.ioctl(rd, BLKFLSBUF, 0)
        back = sha256_of(lambda off, n: os.pread(rd, n, off), size)
    finally:
        os.close(rd)
    if back != img_sha:
        log(f'WRITE VERIFY FAILED: read back {back}, image {img_sha}')
        raise Refused('read-back differs from the image')
    log(f'written and read back: {size} bytes into {a.part} ({node}), sha256 {back}')


def cmd_selftest(_a):
    """the refusals, against the saved GPT of 2026-11-08 (no device needed)"""
    head = open(os.path.join(BACKUP, 'gpt-sda-head.bin'), 'rb').read()
    tail = open(os.path.join(BACKUP, 'gpt-sda-tail.bin'), 'rb').read()
    nb = header(tail[-BS:])['my'] + 1      # the backup header is the disk's last block
    tail_base = nb - len(tail) // BS

    def reader(h, t):
        def read(lba, count):
            if lba + count <= len(h) // BS:
                return h[lba * BS:(lba + count) * BS]
            return t[(lba - tail_base) * BS:(lba - tail_base + count) * BS]
        return read

    ok = True
    ph, ents = read_gpt(reader(head, tail), nb)
    p = check_identity(ph, ents, 'super', EXPECT)
    print(f'saved GPT: good; super = entry {p["number"]}, {p["first"]}..{p["last"]}')
    for what, off in (('a header byte', BS + 60), ('a table byte', 2 * BS + 19 * 128 + 33)):
        bad = bytearray(head)
        bad[off] ^= 1
        try:
            read_gpt(reader(bytes(bad), tail), nb)
            print(f'  NOT refused: {what} changed')
            ok = False
        except Refused as e:
            print(f'  refused ({what} changed): {e}')
    for k, v in (('first', 573439), ('unique', '00000000-0000-0000-0000-000000000000')):
        exp = json.loads(json.dumps(EXPECT))
        exp['parts']['super'][k] = v
        try:
            check_identity(ph, ents, 'super', exp)
            print(f'  NOT refused: expected {k} differs')
            ok = False
        except Refused as e:
            print(f'  refused (expected {k} differs): {e}')
    print('selftest', 'passed' if ok else 'FAILED')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('cmd', choices=['check', 'sum', 'plan', 'write', 'selftest'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--disk')
    ap.add_argument('--part', default='super')
    ap.add_argument('--expect', help='identity JSON from `check --save` (default: the saved 2026-11-08 GPT)')
    ap.add_argument('--save', help='check: write the live identity to this JSON file')
    a = ap.parse_args()
    try:
        if a.cmd == 'selftest':
            sys.exit(0 if cmd_selftest(a) else 1)
        if not a.disk:
            sys.exit('--disk /dev/sdX (the whole LUN 0) is required')
        if a.cmd == 'check':
            cmd_check(a)
        elif a.cmd == 'sum':
            a.block, a.count = int(a.args[0]), int(a.args[1])
            cmd_sum(a)
        else:
            if len(a.args) != 1:
                sys.exit('IMAGE is required')
            a.image = a.args[0]
            cmd_plan(a, write=(a.cmd == 'write'))
    except Refused as e:
        log(f'REFUSED ({a.cmd} {a.part}): {e}')
        sys.exit(1)


if __name__ == '__main__':
    main()
