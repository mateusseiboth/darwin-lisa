#!/usr/bin/env python3
"""The audio DSP's firmware for hakios.adsp.c (adsp=1): pmOS's merged adsp.mbn split into what the driver reads.

The driver (step 2, step_firmware()) reads fw/adsp.mdt and fw/adsp.bNN from the small image or from /fw on the root
(hakios_image_file("fw", ...), then /fw/<name>):
  adsp.mdt  = the ELF32 header (52) + the program headers (32 * phnum) + the hash segment's bytes, at most one page
  adsp.bNN  = the file bytes of loadable segment NN (NN = its program header's index, two digits)
and checks: "\\177ELF\\1", 3..48 program headers, mdt size == 52 + 32*phnum + hashes, every loadable segment inside
the DSP's 64 MB at 0x86700000 (dt: reserved-memory adsp@86700000), the lowest exactly there, each bNN as large as its
header says. `check` repeats those checks here.

The blobs are proprietary and device-local: they stay out of git, in research/hackios-xnu-kernel/fw-adsp/ (like
fw-wifi/). Nothing here writes outside the directories it is given.

  python3 -I fw-adsp-stage.py stage [MBN] [OUT]     split (default: pmOS lisa's adsp.mbn -> fw-adsp/), then check
  python3 -I fw-adsp-stage.py check [DIR]           the driver's checks on a staged directory
  python3 -I fw-adsp-stage.py install DIR DEST      copy adsp.mdt + adsp.bNN into DEST (a root's fw/ directory)
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys

PMOS_MBN = os.path.expanduser("~/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa/lib/firmware/qcom/sm7325/xiaomi/lisa/adsp.mbn")
FW_DIR = "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/fw-adsp"
ADSP_MEMORY, ADSP_MEMORY_SIZE = 0x86700000, 0x04000000
PAGE = 16384                    # the lisa kernel's page (16 KB); the driver refuses a header file larger than a page
MAX_SEGMENTS = 48


def is_hash(flags):
    return (flags >> 24) & 7 == 2


def program_headers(data):
    if len(data) < 52 or data[:5] != b"\x7fELF\x01":
        raise SystemExit("not an ELF32 file")
    phoff, = struct.unpack_from("<I", data, 28)
    phnum, = struct.unpack_from("<H", data, 44)
    if phoff != 52:
        raise SystemExit(f"program headers at {phoff}, not right after the ELF header")
    return [struct.unpack_from("<8I", data, 52 + 32 * i) for i in range(phnum)]


def read_mbn(path):
    if path.endswith(".zst"):
        return subprocess.run(["zstd", "-dc", path], check=True, capture_output=True).stdout
    with open(path, "rb") as f:
        return f.read()


def stage(mbn, out):
    data = read_mbn(mbn)
    headers = program_headers(data)
    os.makedirs(out, exist_ok=True)
    hashes = b""
    written = []
    for i, (ptype, offset, _vaddr, _paddr, filesz, memsz, flags, _align) in enumerate(headers):
        if is_hash(flags):
            hashes = data[offset:offset + filesz]
        elif ptype == 1 and memsz != 0 and filesz != 0:
            name = f"adsp.b{i:02d}"
            with open(os.path.join(out, name), "wb") as f:
                f.write(data[offset:offset + filesz])
            written.append(name)
    with open(os.path.join(out, "adsp.mdt"), "wb") as f:
        f.write(data[:52 + 32 * len(headers)] + hashes)
    sums = []
    for name in ["adsp.mdt"] + written:
        with open(os.path.join(out, name), "rb") as f:
            sums.append(f"{hashlib.sha256(f.read()).hexdigest()}  {name}")
    with open(os.path.join(out, "SHA256SUMS"), "w") as f:
        f.write("\n".join(sums) + "\n")
    with open(os.path.join(out, "SOURCE"), "w") as f:
        f.write(f"{mbn}\nsha256 {hashlib.sha256(data).hexdigest()}  {len(data)} bytes\n")
    print(f"staged {mbn} ({len(data)} bytes) -> {out}: adsp.mdt + {len(written)} segments ({written[0]}..{written[-1]})")


def check(directory):
    with open(os.path.join(directory, "adsp.mdt"), "rb") as f:
        header = f.read()
    headers = program_headers(header + b"\0" * 0)
    count = len(headers)
    problems = []
    if not 3 <= count <= MAX_SEGMENTS:
        problems.append(f"{count} program headers (the driver takes 3..{MAX_SEGMENTS})")
    if len(header) > PAGE:
        problems.append(f"adsp.mdt is {len(header)} bytes, more than a {PAGE}-byte page")
    hashes, lowest, highest, files, segments = 0, None, 0, 0, 0
    for i, (ptype, _offset, _vaddr, paddr, filesz, memsz, flags, _align) in enumerate(headers):
        if is_hash(flags):
            hashes = filesz
        if ptype != 1 or is_hash(flags) or memsz == 0:
            continue
        if paddr < ADSP_MEMORY or filesz > memsz or paddr + memsz > ADSP_MEMORY + ADSP_MEMORY_SIZE:
            problems.append(f"segment {i} ({paddr:#x}, {memsz:#x} bytes) is not within the DSP's memory")
        if filesz:
            path = os.path.join(directory, f"adsp.b{i:02d}")
            size = os.path.getsize(path) if os.path.exists(path) else -1
            if size != filesz:
                problems.append(f"adsp.b{i:02d}: {size} bytes, the header says {filesz}")
            files += max(size, 0)
        lowest = paddr if lowest is None or paddr < lowest else lowest
        highest = max(highest, paddr + memsz)
        segments += 1
    if 52 + 32 * count + hashes != len(header):
        problems.append(f"adsp.mdt has {len(header)} bytes, 52 + 32*{count} + {hashes} = {52 + 32 * count + hashes}")
    if lowest != ADSP_MEMORY:
        problems.append(f"the lowest segment is at {lowest:#x}, not {ADSP_MEMORY:#x}")
    image = ((highest + 0xfff) & ~0xfff) - ADSP_MEMORY
    print(f"{directory}: header {len(header)} bytes ({hashes} of hashes), {count} program headers, {segments} to load, "
          f"{files} bytes of files, image {image:#x} of {ADSP_MEMORY_SIZE:#x} bytes")
    for p in problems:
        print(f"  PROBLEM: {p}")
    print("  OK: what hakios.adsp.c step 2 takes" if not problems else "  NOT what the driver takes")
    return not problems


def install(directory, dest):
    if not check(directory):
        raise SystemExit(1)
    os.makedirs(dest, exist_ok=True)
    names = sorted(n for n in os.listdir(directory) if n == "adsp.mdt" or (n.startswith("adsp.b") and len(n) == 8))
    for name in names:
        shutil.copyfile(os.path.join(directory, name), os.path.join(dest, name))
    print(f"installed {len(names)} files into {dest}")


def main(argv):
    if len(argv) < 2 or argv[1] not in ("stage", "check", "install"):
        raise SystemExit(__doc__)
    if argv[1] == "stage":
        mbn = argv[2] if len(argv) > 2 else PMOS_MBN
        out = argv[3] if len(argv) > 3 else FW_DIR
        stage(mbn, out)
        return 0 if check(out) else 1
    if argv[1] == "check":
        return 0 if check(argv[2] if len(argv) > 2 else FW_DIR) else 1
    if len(argv) != 4:
        raise SystemExit(__doc__)
    install(argv[2], argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
