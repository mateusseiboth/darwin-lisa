#!/usr/bin/env python3
"""darwin-lisa: the peripherals' small firmware files built into the kernel (dlisa.periph_fw.c).

  python3 -I periph-fw.py list               where each file comes from on this PC, its size and sha256
  python3 -I periph-fw.py header OUT         OUT = a C header with every file found (bsd/dev/dlisa.periph_fw.h)

The files are proprietary and device-local (the stock lisa vendor image, pmOS's /lib/firmware); the header is written
into the build tree only (periph-wire.py does it), never into git. A file not found is left out and said so: its driver
then reports it missing at run time.
"""
import hashlib
import os
import subprocess
import sys

HOME = os.path.expanduser("~")
PMOS_FW = HOME + "/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa/lib/firmware"
STOCK_FW = HOME + "/Dev/mobile/stock/super/vendor/firmware"

# name in the kernel -> where it is on this PC (.zst is unpacked)
FILES = {
    "aw8624_haptic.bin": STOCK_FW + "/aw8624_haptic.bin",           # AW8624 RAM waveforms (Xiaomi's driver's name)
    "msbtfw11.mbn": PMOS_FW + "/qca/msbtfw11.mbn.zst",              # WCN6750 Bluetooth rampatch (btqca.c's first try)
    "msbtfw11.tlv": PMOS_FW + "/qca/msbtfw11.tlv.zst",              # ... its TLV form (btqca.c's fallback)
    "msnv11.bin": PMOS_FW + "/qca/msnv11.bin.zst",                  # WCN6750 NVM (board data)
}


def read(path):
    if path.endswith(".zst"):
        return subprocess.run(["zstd", "-dc", path], check=True, capture_output=True).stdout
    with open(path, "rb") as f:
        return f.read()


def found():
    for name, path in FILES.items():
        if os.path.exists(path):
            yield name, path, read(path)
        else:
            print(f"periph-fw: {name}: {path} not on this PC: left out", file=sys.stderr)


def c_array(data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("\t" + "".join(f"0x{b:02x}," for b in data[i:i + 24]))
    return "\n".join(lines)


def header(out):
    parts = ["/* made by kernel/drivers/periph-fw/periph-fw.py: proprietary, device-local, never in git */",
             "struct dlisa_periph_blob {\n\tconst char *name;\n\tconst uint8_t *data;\n\tuint32_t size;\n};"]
    entries = []
    for index, (name, path, data) in enumerate(found()):
        parts.append(f"/* {name}: {path}, {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()} */\n"
                     f"static const uint8_t dlisa_periph_blob{index}[{len(data)}] = {{\n{c_array(data)}\n}};")
        entries.append(f"\t{{ \"{name}\", dlisa_periph_blob{index}, {len(data)} }},")
    entries.append("\t{ NULL, NULL, 0 },")
    parts.append("static const struct dlisa_periph_blob dlisa_periph_blobs[] = {\n" + "\n".join(entries) + "\n};")
    with open(out, "w") as f:
        f.write("\n\n".join(parts) + "\n")
    print(f"periph-fw: {out}: {len(entries) - 1} files")


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "list":
        for name, path, data in found():
            print(f"{name}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}\t{path}")
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "header":
        header(sys.argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
