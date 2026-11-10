#!/usr/bin/env python3
# Re-sign each dyld shared-cache file with 4K-page (pageSize shift 12) SHA256
# code hashes, so a 4K-page kernel accepts the code directory AND the page-in
# hashes validate. Preserves the existing CodeDirectory header (ident, special
# slots, execSeg, flags) and the requirements + CMS blobs verbatim; only the
# code-slot hashes, nCodeSlots, pageSize, CD length, SuperBlob offsets, and the
# header codeSignatureSize are rebuilt.
import sys, struct, os, glob, hashlib

CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0
CSMAGIC_CODEDIRECTORY = 0xfade0c02
PAGE = 4096
PAGESHIFT = 12

def u32(b,o): return struct.unpack(">I",b[o:o+4])[0]

def resign(path):
    with open(path,"r+b") as f:
        hdr = f.read(0x40)
        cso = struct.unpack("<Q", hdr[0x28:0x30])[0]   # codeSignatureOffset
        css = struct.unpack("<Q", hdr[0x30:0x38])[0]   # codeSignatureSize
        f.seek(cso); sb = f.read(css)
        if u32(sb,0) != CSMAGIC_EMBEDDED_SIGNATURE:
            raise RuntimeError("%s: no embedded sig" % path)
        count = u32(sb,8)
        slots = []   # (type, off)
        cd_off = None
        for i in range(count):
            typ = u32(sb,12+i*8); off = u32(sb,12+i*8+4)
            slots.append((typ,off))
            if u32(sb,off)==CSMAGIC_CODEDIRECTORY and cd_off is None:
                cd_off = off
        cd_len = u32(sb, cd_off+4)
        cd = bytearray(sb[cd_off:cd_off+cd_len])
        hashOffset = u32(cd,16)
        hashSize = cd[36]
        hashType = cd[37]
        codeLimit = u32(cd,32)
        if hashType != 2 or hashSize != 32:
            raise RuntimeError("%s: unexpected hashType/size %d/%d" % (path,hashType,hashSize))
        if codeLimit != cso:
            # signed region may differ from codeSignatureOffset; use codeLimit
            pass
        nnew = (codeLimit + PAGE - 1)//PAGE

        # other blobs (requirements, CMS, ...) preserved verbatim, in order
        others = []  # (type, bytes)
        for (typ,off) in slots:
            if off == cd_off: continue
            blen = u32(sb, off+4)
            others.append((typ, bytes(sb[off:off+blen])))

        # new CD: keep [0, hashOffset) verbatim, patch fields, append nnew hashes
        new_cd = bytearray(cd[:hashOffset])
        # patch pageSize (CD-rel 39), nCodeSlots (CD-rel 28), length (CD-rel 4)
        new_cd[39] = PAGESHIFT
        struct.pack_into(">I", new_cd, 28, nnew)
        new_cd_len = hashOffset + nnew*hashSize
        struct.pack_into(">I", new_cd, 4, new_cd_len)

        # compute final codeSignatureSize FIRST (header is inside signed region)
        # superblob: 12 hdr + count*8 index + CD_new + others
        sb_len = 12 + count*8 + new_cd_len + sum(len(b) for _,b in others)
        new_css = ((sb_len + 0xFFF)//0x1000)*0x1000      # 4K-align, keep room
        if new_css < css: new_css = css                  # never shrink below original pad
        # write the final codeSignatureSize into the header BEFORE hashing
        f.seek(0x30); f.write(struct.pack("<Q", new_css))

        # hash the signed region [0, codeLimit) at 4K pages from the (updated) file
        f.seek(0)
        hashes = bytearray()
        remaining = codeLimit
        while remaining > 0:
            n = min(PAGE, remaining)
            page = f.read(n)
            if len(page) != n:
                raise RuntimeError("%s: short read in signed region" % path)
            hashes += hashlib.sha256(page).digest()
            remaining -= n
        assert len(hashes) == nnew*hashSize
        new_cd += hashes
        assert len(new_cd) == new_cd_len

        # rebuild superblob
        blobs = [(0, bytes(new_cd))] + others     # CodeDirectory slot type 0 first
        # slot type for CD is 0; others keep their types
        index = b""
        body = b""
        base = 12 + count*8
        # Must keep the ORIGINAL slot type ordering: CD(0), then others in their order
        off = base
        # CD
        index += struct.pack(">II", 0, off); body += bytes(new_cd); off += len(new_cd)
        for typ,b in others:
            index += struct.pack(">II", typ, off); body += b; off += len(b)
        superblob = struct.pack(">III", CSMAGIC_EMBEDDED_SIGNATURE, base+len(body), count) + index + body
        assert len(superblob) == sb_len, (len(superblob), sb_len)

        # write superblob at codeSignatureOffset, pad file to cso+new_css
        f.seek(cso)
        f.write(superblob)
        pad = new_css - len(superblob)
        if pad > 0:
            f.write(b"\x00"*pad)
        f.truncate(cso + new_css)
    return nnew, codeLimit, new_css

def main():
    d = sys.argv[1]
    files = sorted(glob.glob(os.path.join(d,"dyld_shared_cache_arm64e*")))
    files = [f for f in files if ".symbols" not in os.path.basename(f)]
    for f in files:
        nnew, cl, css = resign(f)
        print("resigned %-40s nCodeSlots=%d codeLimit=0x%x codeSigSize=0x%x" %
              (os.path.basename(f), nnew, cl, css))
    print("done: %d files" % len(files))

main()
