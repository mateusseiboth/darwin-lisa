#!/usr/bin/env python3
"""Stage the Adreno 642L firmware for hakios.adreno.c (boot-arg gpu=1).

The driver reads four files from fw/ of a RAM disk (hakios.image.c: md1, else md0 = the iOS 26 system image):
  a660_gmu.bin   the GMU's firmware            (step 14)
  a660_sqe.fw    the command processor's       (step 18)
  a660_zap.mdt   the zap shader's header: ELF32 header + program headers + the hash segment, nothing else
                 (zap_header(): size == 52 + 32*phnum + hashes, size <= PAGE_SIZE)
  a660_zap.b02   the zap shader's one loadable segment, relocatable, <= 0x1000 bytes (step 19)

  stage   [--from PMOS_FIRMWARE_DIR] [--out DIR]
          copies the pmOS set (postmarketOS runs this phone's GPU with freedreno) into DIR, default
          research/hackios-xnu-kernel/fw-gpu/pmos/: a660_gmu.bin and a660_sqe.fw from qcom/, the board's signed zap
          qcom/sm7325/xiaomi/lisa/a660_zap.mbn split into .mdt + .bNN (every segment with file data, as the stock
          split is). A .zst of any of them is decompressed first. The proven stock set in fw-gpu/ is never touched.
  check   DIR    the driver's own checks on a set, and how it compares with the stock one
  install DIR DEST
          copies the four files the driver reads from DIR into DEST (the image's fw/, for instance
          ~/Dev/mobile/userland-26/out/sysroot-26/fw), then the image is rebuilt (userland-26: scripts/mkimage-trim.sh)

Proprietary blobs: nothing here is committed. The pmOS chroot is ~/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys

M = os.path.expanduser("~/Dev/mobile")
PMOS = os.path.expanduser("~/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa/lib/firmware")
STOCK = os.path.join(M, "research/hackios-xnu-kernel/fw-gpu")
DRIVER_FILES = ("a660_gmu.bin", "a660_sqe.fw", "a660_zap.mdt", "a660_zap.b02")
KERNEL_PAGE = 16384     # this kernel's PAGE_SIZE (zap_header refuses a bigger .mdt)
ZAP_SEGMENT_MAX = 0x1000


def readFirmware(path):
    """The file, or its .zst decompressed; None when neither is there."""
    if os.path.exists(path):
        return open(path, "rb").read()
    if os.path.exists(path + ".zst"):
        return subprocess.run(["zstd", "-dcq", path + ".zst"], check=True, capture_output=True).stdout
    return None


def splitZap(mbn):
    """(mdt bytes, {index: segment bytes}) of a merged Qualcomm PIL image."""
    if mbn[:5] != b"\x7fELF\x01":
        sys.exit("fw-gpu-stage: the zap shader is not an ELF32 image")
    phoff, = struct.unpack_from("<I", mbn, 28)
    phnum, = struct.unpack_from("<H", mbn, 44)
    if phoff != 52:
        sys.exit(f"fw-gpu-stage: program headers at {phoff}, not right after the ELF header")
    hashes = b""
    segments = {}
    for i in range(phnum):
        _type, offset, _vaddr, _paddr, filesz, _memsz, flags, _align = struct.unpack_from("<8I", mbn, 52 + 32 * i)
        data = mbn[offset:offset + filesz]
        if ((flags >> 24) & 7) == 2:
            hashes = data
        if filesz:
            segments[i] = data
    return mbn[:52 + 32 * phnum] + hashes, segments


def sha(data):
    return hashlib.sha256(data).hexdigest()


def stage(source, out):
    gmu = readFirmware(os.path.join(source, "qcom/a660_gmu.bin"))
    sqe = readFirmware(os.path.join(source, "qcom/a660_sqe.fw"))
    zap = readFirmware(os.path.join(source, "qcom/sm7325/xiaomi/lisa/a660_zap.mbn"))
    missing = [n for n, d in (("a660_gmu.bin", gmu), ("a660_sqe.fw", sqe), ("a660_zap.mbn", zap)) if d is None]
    if missing:
        sys.exit(f"fw-gpu-stage: not in {source}: {', '.join(missing)}")
    if os.path.realpath(out) == os.path.realpath(STOCK):
        sys.exit("fw-gpu-stage: refusing to write over the proven stock set in fw-gpu/")
    os.makedirs(out, exist_ok=True)
    mdt, segments = splitZap(zap)
    files = {"a660_gmu.bin": gmu, "a660_sqe.fw": sqe, "a660_zap.mbn": zap, "a660_zap.mdt": mdt}
    files.update({f"a660_zap.b{i:02d}": data for i, data in segments.items()})
    for name, data in sorted(files.items()):
        open(os.path.join(out, name), "wb").write(data)
    with open(os.path.join(out, "SHA256SUMS"), "w") as f:
        for name, data in sorted(files.items()):
            f.write(f"{sha(data)}  {name}\n")
    print(f"fw-gpu-stage: {len(files)} files in {out}")
    return check(out)


def check(directory):
    """The checks hakios.adreno.c makes, on the four files it reads. 0 when they pass."""
    failures = 0
    data = {n: open(os.path.join(directory, n), "rb").read() for n in DRIVER_FILES
            if os.path.exists(os.path.join(directory, n))}
    for name in DRIVER_FILES:
        if name not in data:
            print(f"  FAIL {name}: missing (the driver says 'THE GPU IS NOT STARTED: the small image has no fw/{name}')")
            failures += 1
    mdt = data.get("a660_zap.mdt", b"")
    if mdt:
        phnum = mdt[44] | mdt[45] << 8 if len(mdt) >= 52 + 2 * 32 else 0
        hashes = struct.unpack_from("<I", mdt, 52 + 32 + 16)[0] if phnum else 0
        ok = mdt[:5] == b"\x7fELF\x01" and phnum >= 2 and len(mdt) <= KERNEL_PAGE and 52 + 32 * phnum + hashes == len(mdt)
        print(f"  {'ok  ' if ok else 'FAIL'} a660_zap.mdt: {len(mdt)} bytes = 52 + 32*{phnum} + {hashes} of hashes")
        failures += not ok
    seg = data.get("a660_zap.b02", b"")
    if seg:
        ok = 0 < len(seg) <= ZAP_SEGMENT_MAX
        print(f"  {'ok  ' if ok else 'FAIL'} a660_zap.b02: {len(seg)} bytes (at most {ZAP_SEGMENT_MAX})")
        failures += not ok
    for name in ("a660_gmu.bin", "a660_sqe.fw"):
        if name in data:
            print(f"  ok   {name}: {len(data[name])} bytes")
    for name in DRIVER_FILES:
        stock = os.path.join(STOCK, name)
        if name in data and os.path.exists(stock) and os.path.realpath(stock) != os.path.realpath(os.path.join(directory, name)):
            same = open(stock, "rb").read() == data[name]
            print(f"       {name}: {'the same as' if same else 'differs from'} the stock fw-gpu/{name}")
    print(f"fw-gpu-stage: {directory}: {'passes' if failures == 0 else f'{failures} failures'}")
    return failures


def install(directory, dest):
    if check(directory):
        sys.exit("fw-gpu-stage: not installing a set the driver would refuse")
    os.makedirs(dest, exist_ok=True)
    for name in DRIVER_FILES:
        shutil.copyfile(os.path.join(directory, name), os.path.join(dest, name))
        print(f"  {name} -> {dest}")
    print("fw-gpu-stage: now rebuild the image (userland-26: LEAN=1 bash scripts/mkimage-trim.sh)")


def main(argv):
    if len(argv) >= 1 and argv[0] == "stage":
        source, out = PMOS, os.path.join(STOCK, "pmos")
        rest = argv[1:]
        while rest:
            if rest[0] == "--from" and len(rest) > 1:
                source, rest = rest[1], rest[2:]
            elif rest[0] == "--out" and len(rest) > 1:
                out, rest = rest[1], rest[2:]
            else:
                sys.exit(__doc__)
        return stage(source, out)
    if len(argv) == 2 and argv[0] == "check":
        return check(argv[1])
    if len(argv) == 3 and argv[0] == "install":
        return install(argv[1], argv[2])
    sys.exit(__doc__)


sys.exit(1 if main(sys.argv[1:]) else 0)
