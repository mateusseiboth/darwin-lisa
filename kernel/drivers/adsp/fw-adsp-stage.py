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
  python3 -I fw-adsp-stage.py header DIR OUT        OUT = a C header that builds DIR's files into the kernel
                                                    (adsp-wire.py: bsd/dev/hakios.adsp_fw.h; hakios.adsp.c image_file())
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
    names = firmware_names(directory)
    for name in names:
        shutil.copyfile(os.path.join(directory, name), os.path.join(dest, name))
    print(f"installed {len(names)} files into {dest}")


def firmware_names(directory):
    """adsp.mdt first, then adsp.bNN in order: what install copies and header builds in."""
    return ["adsp.mdt"] + sorted(n for n in os.listdir(directory) if n.startswith("adsp.b") and len(n) == 8)


def header(directory, out):
    """A header that puts DIR's files into the kernel with .incbin (by DIR's absolute path, read when it is compiled)
    and lists them in adsp_builtin_fw[]: the root then needs no 20 MB of /fw (the root's size budget). Each file
    starts on a 4 KB boundary, as a file of a RAM disk does. Same input, same bytes: the sha256 of each file is in the
    header, so a new firmware recompiles the file that includes it."""
    if not check(directory):
        raise SystemExit(1)
    directory = os.path.abspath(directory)
    rows, asm, decls = [], [], []
    for i, name in enumerate(firmware_names(directory)):
        path = os.path.join(directory, name)
        with open(path, "rb") as f:
            data = f.read()
        if '"' in path or "\\" in path:
            raise SystemExit(f"{path}: a quote or backslash in the path")
        sym = f"dlisa_adsp_fw_{i}"
        asm.append("\t" + r'"\t.p2align 12\n" DLISA_ADSP_FW_GLOBAL(' + sym + r') DLISA_ADSP_FW_SYM(' + sym + r') ":\n"')
        asm.append("\t" + r'"\t.incbin \"' + path + r'\"\n"')
        decls.append(f'extern const uint8_t {sym}[] __attribute__((visibility("hidden")));')
        rows.append(f'\t{{ "{name}", {sym}, {len(data)} }},  /* sha256 {hashlib.sha256(data).hexdigest()} */')
    text = "\n".join([
        f"/* darwin-lisa: generated by kernel/drivers/adsp/fw-adsp-stage.py header from {directory}.",
        " * Qualcomm firmware, assembled in with .incbin: never commit this header or those files. */",
        "#define HAKIOS_ADSP_BUILTIN_FW 1",
        "#define DLISA_ADSP_FW_STR2(x) #x",
        "#define DLISA_ADSP_FW_STR(x) DLISA_ADSP_FW_STR2(x)",
        "#define DLISA_ADSP_FW_SYM(n) DLISA_ADSP_FW_STR(__USER_LABEL_PREFIX__) #n",
        "#ifdef __APPLE__",
        r'#define DLISA_ADSP_FW_SECTION "\t.section __TEXT,__const\n"',
        r'#define DLISA_ADSP_FW_GLOBAL(n) "\t.globl " DLISA_ADSP_FW_SYM(n) "\n\t.private_extern " DLISA_ADSP_FW_SYM(n) "\n"',
        "#else",
        r'#define DLISA_ADSP_FW_SECTION "\t.section .rodata\n"',
        r'#define DLISA_ADSP_FW_GLOBAL(n) "\t.globl " DLISA_ADSP_FW_SYM(n) "\n\t.hidden " DLISA_ADSP_FW_SYM(n) "\n"',
        "#endif",
        "__asm__(DLISA_ADSP_FW_SECTION",
        *asm,
        "\t" + r'"\t.text\n");',
        *decls,
        "static const struct { const char *name; const uint8_t *data; uint64_t size; } adsp_builtin_fw[] = {",
        *rows,
        "};",
        "",
    ])
    if os.path.exists(out) and open(out).read() == text:
        print(f"{out}: the same already")
        return
    with open(out, "w") as f:
        f.write(text)
    print(f"{out}: {len(rows)} files of {directory} built in")


def main(argv):
    if len(argv) < 2 or argv[1] not in ("stage", "check", "install", "header"):
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
    if argv[1] == "header":
        header(argv[2], argv[3])
        return 0
    install(argv[2], argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
