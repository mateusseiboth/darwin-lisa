#!/usr/bin/env python3
"""Strip pointer authentication from the CODE of a split dyld shared cache, in place,
and re-hash the code signature of every file it changes.

Why offline and code-only: the runtime alternative (kernel phase 3, the apple-protect
pager over every executable cache mapping) rewrites whole pages, so data that lives in
executable segments is rewritten too wherever a word happens to encode a PAC
instruction. The first casualty was the cache's own image table: WiFiCloudSyncEngine's
address 0x2_9AD43000 has a low word 0x9AD43000 = "pacga x0, x0, x20", which became
"eor ..., ror #13" = 0xCAD43400, so dyld handed libobjc a mach_header at
0x2_CAD43400 and SpringBoard / UserEventAgent died with SIGBUS in map_images. In one
sub-cache alone ~900 data words outside any code section match.

Here only the sections each image marks as instructions (S_ATTR_PURE_INSTRUCTIONS /
S_ATTR_SOME_INSTRUCTIONS) are rewritten, with darwin-lisa/tools/pacstrip/insn.py (the
same table the kernel's pac_rewrite.c uses). Instructions with no 4-byte replacement
are left in place for the kernel's EL0 emulator. Then each changed file's
CodeDirectory code-slot hashes are recomputed (same page size and hash type), so
the kernel's page-in validation accepts the pages.

A file that is a hard link (shared with a pristine backup) is first replaced by a
private copy, so the backup stays pristine.

Also the cache's stub islands (__TEXT_STUBS mappings), which no image lists.

usage: pacstrip-cache.py CACHE_DIR
"""
import collections
import hashlib
import os
import shutil
import struct
import sys

import numpy as np

sys.path.insert(0, "/home/mateusseiboth/Dev/mobile/darwin-lisa/tools/pacstrip")
from insn import PATCH, TRAP, rewrite  # noqa: E402

BASE = "dyld_shared_cache_arm64e"
CODE = 0x80000000 | 0x00000400
DYLD_CACHE_MAPPING_TEXT_STUBS = 0x8
CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CSMAGIC_CODEDIRECTORY = 0xFADE0C02
CANDIDATE_TOP_BYTES = np.array([0xD6, 0xD7, 0xDA, 0x9A, 0xF8, 0x1E], dtype=np.uint32)
HASHES = {1: hashlib.sha1, 2: hashlib.sha256, 3: hashlib.sha256, 4: hashlib.sha384}


def u32le(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u32be(b, o):
    return struct.unpack_from(">I", b, o)[0]


def load(cachedir):
    main = open(os.path.join(cachedir, BASE), "rb").read(0x80000)
    off, cnt = u32le(main, 0x188), u32le(main, 0x18C)
    files = [BASE] + [BASE + main[off + i * 56 + 24:off + i * 56 + 56].split(b"\0")[0].decode() for i in range(cnt)]
    maps = []
    for f in files:
        h = open(os.path.join(cachedir, f), "rb").read(0x4000)
        mo, mc = u32le(h, 0x10), u32le(h, 0x14)
        for j in range(mc):
            a, s, fo, _mp, ip = struct.unpack_from("<QQQII", h, mo + j * 32)
            maps.append((a, s, fo, f, ip))
    io, ic = u32le(main, 0x1C0), u32le(main, 0x1C4)
    images = [struct.unpack_from("<Q", main, io + i * 32)[0] for i in range(ic)]
    return maps, images


def v2f(maps, addr):
    for a, s, fo, f, _ip in maps:
        if a <= addr < a + s:
            return f, addr - a + fo
    raise ValueError("address %#x not in any cache mapping" % addr)


def code_ranges(cachedir, maps, images):
    handles = {}
    ranges = collections.defaultdict(list)
    for addr in images:
        f, o = v2f(maps, addr)
        fh = handles.get(f) or handles.setdefault(f, open(os.path.join(cachedir, f), "rb"))
        fh.seek(o)
        hdr = fh.read(0x10000)
        magic, _cpu, _sub, _ft, ncmds, _sz = struct.unpack_from("<IiiIII", hdr, 0)
        if magic != 0xFEEDFACF:
            raise ValueError("image at %#x: bad magic %#x" % (addr, magic))
        lc = 32
        for _ in range(ncmds):
            cmd, csz = struct.unpack_from("<II", hdr, lc)
            if cmd == 0x19:  # LC_SEGMENT_64
                for k in range(u32le(hdr, lc + 64)):
                    s = lc + 72 + 80 * k
                    saddr, ssize = struct.unpack_from("<QQ", hdr, s + 32)
                    if u32le(hdr, s + 64) & CODE and ssize:
                        sf, so = v2f(maps, saddr)
                        ranges[sf].append((so, ssize))
            lc += csz
    for fh in handles.values():
        fh.close()
    # The cache's own stub islands (__TEXT_STUBS mappings: auth stubs ending in BRAA x16, x17)
    # belong to no image's section list. Their mapping starts at file offset 0, but the first
    # 16 KB page is the sub-cache's header: the stubs begin at 0x4000.
    for f in {m[3] for m in maps}:
        h = open(os.path.join(cachedir, f), "rb").read(0x4000)
        mo, mc = u32le(h, 0x138), u32le(h, 0x13C)
        for j in range(mc):
            _a, size, fo, _sio, _sis, flags, _mp, ip = struct.unpack_from("<QQQQQQII", h, mo + j * 56)
            if ip & 4 and flags & DYLD_CACHE_MAPPING_TEXT_STUBS:
                start = max(fo, 0x4000)
                if fo + size > start:
                    ranges[f].append((start, fo + size - start))
    return ranges


def private_copy(path):
    """Replace a hard link by a file of its own, so the other names keep the original."""
    if os.stat(path).st_nlink > 1:
        tmp = path + ".pacstrip-tmp"
        shutil.copyfile(path, tmp)
        shutil.copymode(path, tmp)
        os.replace(tmp, path)


def rehash(data):
    """Recompute every CodeDirectory's code-slot hashes in the file's own signature."""
    cso, css = struct.unpack_from("<QQ", data, 0x28)
    sb = cso
    if u32be(data, sb) != CSMAGIC_EMBEDDED_SIGNATURE:
        raise ValueError("no embedded signature")
    changed = 0
    for i in range(u32be(data, sb + 8)):
        _typ, boff = struct.unpack_from(">II", data, sb + 12 + i * 8)
        cd = sb + boff
        if u32be(data, cd) != CSMAGIC_CODEDIRECTORY:
            continue
        hash_offset, nslots, limit = u32be(data, cd + 16), u32be(data, cd + 28), u32be(data, cd + 32)
        hsize, htype, pshift = data[cd + 36], data[cd + 37], data[cd + 39]
        algo, page = HASHES[htype], 1 << pshift
        for slot in range(nslots):
            start = slot * page
            digest = algo(bytes(data[start:min(start + page, limit)])).digest()[:hsize]
            at = cd + hash_offset + slot * hsize
            if data[at:at + hsize] != digest:
                data[at:at + hsize] = digest
                changed += 1
    return changed


def main():
    cachedir = sys.argv[1]
    maps, images = load(cachedir)
    ranges = code_ranges(cachedir, maps, images)
    total_p = total_t = 0
    for f in sorted(ranges):
        path = os.path.join(cachedir, f)
        private_copy(path)
        data = bytearray(open(path, "rb").read())
        patched = traps = 0
        for off, size in ranges[f]:
            words = np.frombuffer(data, dtype="<u4", count=size // 4, offset=off)
            # only words whose top byte starts a PAC encoding (see insn.STRATEGIES) can change
            top = words >> 24
            for i in np.nonzero(np.isin(top, CANDIDATE_TOP_BYTES))[0]:
                at = off + int(i) * 4
                kind, new = rewrite(int(words[i]))
                if kind == PATCH:
                    struct.pack_into("<I", data, at, new)
                    patched += 1
                elif kind == TRAP:
                    traps += 1
        slots = rehash(data)
        with open(path, "r+b") as fh:
            fh.write(data)
        total_p += patched
        total_t += traps
        print("%s: %d sections, %d patched, %d left for the emulator, %d code-slot hashes updated"
              % (f, len(ranges[f]), patched, traps, slots), flush=True)
    print("total: %d patched, %d left for the emulator" % (total_p, total_t))


if __name__ == "__main__":
    main()
