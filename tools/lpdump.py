#!/usr/bin/env python3
"""Read-only dump of Android's dynamic-partition (LP) metadata in a `super` partition.

usage: lpdump.py SUPER [--slot N] [--json]

SUPER is a raw block device or image (/dev/sdX20 through U-Boot's ums, /dev/disk/by-partlabel/super on pmOS) or an
Android sparse image (the factory super.img). Only the first few MiB are read; nothing is ever written.

Prints super's size as the geometry says, every logical partition with its group, attributes, size and extents (in
512-byte sectors of super and in absolute 4 KiB blocks of the unit when --super-first-lba is given), the groups and the
free space left in the block device. The format is liblp's (system/core/fs_mgr/liblp/include/liblp/metadata_format.h):
4 KiB reserved, the geometry (twice, 4 KiB each), then metadata slots of `metadata_max_size`, primary then backup.
"""
import argparse
import json
import struct
import sys

LP_RESERVED = 4096
GEOMETRY_MAGIC = 0x616C4467
HEADER_MAGIC = 0x414C5030
SPARSE_MAGIC = 0xED26FF3A
SECTOR = 512


class Source:
    """random access to the first bytes of a raw device or of an Android sparse image, read only"""

    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(28)
        self.sparse = struct.unpack_from('<I', head, 0)[0] == SPARSE_MAGIC
        if self.sparse:
            self._index(head)

    def _index(self, head):
        _, _, _, fhdr, chdr, blk, total_blks, total_chunks, _ = struct.unpack('<IHHHHIIII', head)
        self.blk = blk
        self.chunks = []           # (first output block, block count, kind, file offset or fill)
        self.f.seek(fhdr)
        out = 0
        for _ in range(total_chunks):
            ctype, _, nblk, total = struct.unpack('<HHII', self.f.read(12))
            self.f.seek(chdr - 12, 1)
            data_at = self.f.tell()
            body = total - chdr
            if ctype == 0xCAC1:
                self.chunks.append((out, nblk, 'raw', data_at))
            elif ctype == 0xCAC2:
                self.chunks.append((out, nblk, 'fill', self.f.read(4)))
                body -= 4
            else:
                self.chunks.append((out, nblk, 'zero', None))
            self.f.seek(body, 1)
            out += nblk
            if out * blk > 64 << 20:     # the metadata is at the start: 64 MiB of index is plenty
                break

    def read(self, off, n):
        if not self.sparse:
            self.f.seek(off)
            data = self.f.read(n)
            if len(data) != n:
                sys.exit(f'lpdump.py: short read at {off}')
            return data
        out = bytearray()
        while n > 0:
            b, inner = divmod(off, self.blk)
            for first, cnt, kind, val in self.chunks:
                if first <= b < first + cnt:
                    take = min(n, (first + cnt - b) * self.blk - inner)
                    if kind == 'raw':
                        self.f.seek(val + (b - first) * self.blk + inner)
                        out += self.f.read(take)
                    elif kind == 'fill':
                        out += (val * ((take + inner) // 4 + 2))[inner % 4:inner % 4 + take]
                    else:
                        out += bytes(take)
                    break
            else:
                sys.exit(f'lpdump.py: offset {off} is past the indexed part of the sparse image')
            off += take
            n -= take
        return bytes(out)


def parse(src, slot):
    g = src.read(LP_RESERVED, 4096)
    # LpMetadataGeometry: magic, struct_size, checksum[32], metadata_max_size, metadata_slot_count, logical_block_size
    magic, _size, meta_max, slots, lblock = struct.unpack_from('<II32xIII', g)
    if magic != GEOMETRY_MAGIC:
        sys.exit('lpdump.py: no LP geometry at 4096 (not a super partition, or not the first bytes of one)')
    meta_at = LP_RESERVED + 2 * 4096 + slot * meta_max
    h = src.read(meta_at, meta_max)
    if struct.unpack_from('<I', h, 0)[0] != HEADER_MAGIC:
        sys.exit(f'lpdump.py: no metadata header in slot {slot}')
    major, minor, hsize = struct.unpack_from('<HHI', h, 4)
    tables_size = struct.unpack_from('<I', h, 44)[0]
    descs = []
    at = 80
    for _ in range(4):     # partitions, extents, groups, block devices
        descs.append(struct.unpack_from('<III', h, at))
        at += 12
    tables = h[hsize:hsize + tables_size]

    def table(i):
        off, num, esz = descs[i]
        return [tables[off + k * esz: off + (k + 1) * esz] for k in range(num)]

    extents = []
    for e in table(1):
        nsec, etype, target, tsrc = struct.unpack_from('<QIQI', e)
        extents.append({'sectors': nsec, 'type': 'linear' if etype == 0 else 'zero', 'start_sector': target,
                        'block_device': tsrc})
    groups = []
    for gr in table(2):
        groups.append({'name': gr[:36].split(b'\0', 1)[0].decode(), 'max_size': struct.unpack_from('<Q', gr, 40)[0]})
    devices = []
    for d in table(3):
        first_sec, align, align_off, size = struct.unpack_from('<QIIQ', d)
        devices.append({'name': d[24:60].split(b'\0', 1)[0].decode(), 'first_logical_sector': first_sec,
                        'alignment': align, 'size': size})
    parts = []
    for p in table(0):
        name = p[:36].split(b'\0', 1)[0].decode()
        attrs, first_ext, n_ext, group = struct.unpack_from('<IIII', p, 36)
        ex = extents[first_ext:first_ext + n_ext]
        parts.append({'name': name, 'group': groups[group]['name'] if group < len(groups) else group,
                      'attributes': attrs, 'bytes': sum(x['sectors'] for x in ex) * SECTOR, 'extents': ex})
    return {'version': f'{major}.{minor}', 'metadata_max_size': meta_max, 'metadata_slots': slots,
            'logical_block_size': lblock, 'block_devices': devices, 'groups': groups, 'partitions': parts}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('super')
    ap.add_argument('--slot', type=int, default=0, help='metadata slot (0: slot a, 1: slot b)')
    ap.add_argument('--super-first-lba', type=int,
                    help="super's first LBA on its unit, in 4 KiB blocks (sgdisk -i 20): also prints absolute blocks")
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    m = parse(Source(a.super), a.slot)
    if a.json:
        print(json.dumps(m, indent=1))
        return
    dev = m['block_devices'][0] if m['block_devices'] else {'size': 0, 'first_logical_sector': 0}
    print(f"LP metadata {m['version']}, {m['metadata_slots']} slots of {m['metadata_max_size']} bytes")
    for d in m['block_devices']:
        print(f"block device {d['name']}: {d['size']} bytes ({d['size'] / 2**30:.2f} GiB), "
              f"first logical sector {d['first_logical_sector']}, alignment {d['alignment']}")
    for gr in m['groups']:
        print(f"group {gr['name']}: max {gr['max_size'] / 2**30:.2f} GiB")
    used = 0
    for p in m['partitions']:
        flags = ('readonly ' if p['attributes'] & 1 else '') + ('slot-suffixed ' if p['attributes'] & 2 else '') + \
                ('updated ' if p['attributes'] & 4 else '') + ('disabled ' if p['attributes'] & 8 else '')
        print(f"  {p['name']:24s} {p['group']:20s} {p['bytes']:>12d} B ({p['bytes'] / 2**30:6.2f} GiB) {flags}")
        for x in p['extents']:
            line = f"      {x['type']} {x['sectors']} sectors from sector {x['start_sector']} (dev {x['block_device']})"
            if x['type'] == 'linear':
                used += x['sectors'] * SECTOR
                if a.super_first_lba is not None and x['start_sector'] % 8 == 0 and x['sectors'] % 8 == 0:
                    first = a.super_first_lba + x['start_sector'] // 8
                    line += f"  = unit blocks {first}..{first + x['sectors'] // 8 - 1}"
            print(line)
    first_free = dev['first_logical_sector'] * SECTOR
    print(f"linear extents total {used / 2**30:.2f} GiB; data area {(dev['size'] - first_free) / 2**30:.2f} GiB; "
          f"free {(dev['size'] - first_free - used) / 2**30:.2f} GiB")


if __name__ == '__main__':
    main()
