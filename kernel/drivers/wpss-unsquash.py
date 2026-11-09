#!/usr/bin/env python3
"""Un-squash a merged Qualcomm pil .mbn into the split .mdt + .bNN the HakiOS
WiFi driver (hakios.wifi.c) expects:
  - wpss.mdt  = ELF32 header (52) + program-header table (32*phnum) + the hash segment
  - wpss.bNN  = each loadable segment's file data, NN = program-header index
The driver's check is  mdt_size == 52 + 32*phnum + hashes  (hashes = hash seg file_size).
ELF32 phdr: type@0, offset@4, vaddr@8, paddr@12, filesz@16, memsz@20, flags@24, align@28.
Hash segment: ((flags>>24)&7)==2.  Loadable: type==1 and not hash and memsz!=0.
"""
import struct, sys, os

mbn, outdir = sys.argv[1], sys.argv[2]
data = open(mbn, "rb").read()
assert data[:4] == b"\x7fELF" and data[4] == 1, "not an ELF32"
e_phoff, = struct.unpack_from("<I", data, 28)
e_phnum, = struct.unpack_from("<H", data, 44)
assert e_phoff == 52, f"unexpected e_phoff {e_phoff}"
os.makedirs(outdir, exist_ok=True)

hashes = 0
blobs = []  # (index, bytes)
for i in range(e_phnum):
    p = e_phoff + 32 * i
    ptype, off, _vaddr, paddr, filesz, memsz, flags, _align = struct.unpack_from("<8I", data, p)
    is_hash = ((flags >> 24) & 7) == 2
    seg = data[off:off + filesz]
    if is_hash:
        hashes = filesz
        hash_bytes = seg
    elif ptype == 1 and memsz != 0 and filesz != 0:
        blobs.append((i, seg))

mdt = data[:52 + 32 * e_phnum] + (hash_bytes if hashes else b"")
open(os.path.join(outdir, "wpss.mdt"), "wb").write(mdt)
for i, seg in blobs:
    open(os.path.join(outdir, f"wpss.b{i:02d}"), "wb").write(seg)

check = 52 + 32 * e_phnum + hashes
print(f"phnum={e_phnum} hashes={hashes}")
print(f"wpss.mdt = {len(mdt)} bytes; driver expects 52+32*{e_phnum}+{hashes} = {check}  -> {'OK' if len(mdt)==check else 'MISMATCH'}")
print("segments written:", ", ".join(f"wpss.b{i:02d}({len(s)})" for i, s in blobs))
