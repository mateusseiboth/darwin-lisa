#!/usr/bin/env python3
"""The files the sensors PD on lisa's audio DSP reads from the host through FastRPC (dlisa.fastrpc.c, sensors_rpc=1),
staged and packed into the archive that is linked into the kernel.

What the DSP reads (its firmware's strings and Android's own paths, kernel/drivers/adsp/README.md "Sensors: FastRPC"):
  /vendor/etc/sensors/sns_reg_config               which files it reads, and where the registry goes
  /vendor/etc/sensors/config/*.json                the drivers' and algorithms' configuration (57 files, 156 KB)
  /mnt/vendor/persist/sensors/registry/registry/*  the registry: the JSON applied to this board, with the calibration
  /mnt/vendor/persist/sensors/registry/sns_reg_version (and the rest of persist's sensors/, when there is a dump of it)
  /sys/devices/soc0/{hw_platform,soc_id,...}       which JSON files apply (their "config" header)

Sources, each a directory on this computer (nothing is fetched and nothing is written outside --out):
  --vendor DIR    the stock vendor partition, extracted (default ~/Dev/mobile/stock/super/vendor): etc/sensors/ minus
                  proto/ and scripts/, which the DSP never reads
  --persist DIR   a read-only dump of the phone's own persist/sensors/ (`dump-persist`): its registry carries THIS
                  phone's calibration. Wins over --registry. Default: OUT/persist-dump/sensors when it is there
  --registry DIR  a registry directory (default: the pmOS firmware package's /usr/share/qcom/sensors/registry, another
                  lisa's, calibration included). Used only without a persist dump
  --socinfo DIR   socinfo values (default: the same package's /usr/share/qcom/socinfo: QRD, 475, ...)
The blobs are proprietary and device-local: they stay out of git, in research/hackios-xnu-kernel/fw-sensors/.

  python3 -I fw-sensors-stage.py stage [--vendor DIR] [--persist DIR] [--registry DIR] [--socinfo DIR] [--out DIR]
  python3 -I fw-sensors-stage.py check [OUT]               the archive against the tree, and the DSP's must-haves
  python3 -I fw-sensors-stage.py header OUT HEADER         HEADER = a C header that links OUT/sensors.fs into the kernel
  python3 -I fw-sensors-stage.py dump-persist [--host USER@HOST] [--out OUT]
                       from pmOS over SSH: persist mounted READ-ONLY (ro,noload: not even the journal is replayed) or
                       read where pmOS already mounts it, sensors/ copied out as a tar into OUT/persist-dump/. Never
                       writes the partition. Run it with the phone in pmOS (darwin-lisa/tools/bootos.sh pmos).

The archive (dlisa.fastrpc_proto.h reads it): "DLSNSFS1", u32 count, u32 total size, then per file {u32 path offset,
u32 path length, u32 data offset, u32 data length}, then the paths (NUL-terminated) and the data; paths absolute and
sorted, little-endian.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tarfile

MOBILE = "/home/mateusseiboth/Dev/mobile"
PMOS_QCOM = os.path.expanduser("~/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa/usr/share/qcom")
DEFAULTS = {
    "vendor": MOBILE + "/stock/super/vendor",
    "registry": PMOS_QCOM + "/sensors/registry",
    "socinfo": PMOS_QCOM + "/socinfo",
    "out": MOBILE + "/research/hackios-xnu-kernel/fw-sensors",
}
REGISTRY = "mnt/vendor/persist/sensors/registry/registry"
PERSIST_SENSORS = "mnt/vendor/persist/sensors"
SOCINFO = "sys/devices/soc0"
VENDOR_SENSORS = "vendor/etc/sensors"
VENDOR_SKIP = {"proto", "scripts"}
MUST_HAVE = ["vendor/etc/sensors/sns_reg_config", "sys/devices/soc0/soc_id", "sys/devices/soc0/hw_platform"]
MAGIC = b"DLSNSFS1"
LIMIT = 4 << 20                 # the archive goes into the kernel: refuse anything unexpectedly large
PATH_MAX = 255                  # FRPC_PATH - 1


def fail(text):
    raise SystemExit(f"fw-sensors-stage: {text}")


def copy_tree(src, dst, skip=frozenset()):
    """Regular files and directories only; symlinks and devices are refused rather than followed."""
    if not os.path.isdir(src):
        fail(f"{src}: not a directory")
    os.makedirs(dst, exist_ok=True)
    for name in sorted(os.listdir(src)):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if name in skip:
            continue
        if os.path.islink(s):
            fail(f"{s}: a symlink in a source tree")
        if os.path.isdir(s):
            copy_tree(s, d)
        elif os.path.isfile(s):
            shutil.copyfile(s, d)
        else:
            fail(f"{s}: neither a file nor a directory")


def files_of(root):
    found = []
    for directory, dirs, names in os.walk(root):
        dirs.sort()
        for name in sorted(names):
            path = os.path.join(directory, name)
            found.append(("/" + os.path.relpath(path, root).replace(os.sep, "/"), path))
    return sorted(found)


def pack(root):
    files = files_of(root)
    head = 16 + 16 * len(files)
    entries, blob, at = [], bytearray(), head
    for name, path in files:
        raw = name.encode()
        if len(raw) > PATH_MAX or b"\0" in raw:
            fail(f"{name}: a path the kernel cannot take")
        with open(path, "rb") as f:
            data = f.read()
        path_at = at
        blob += raw + b"\0"
        at += len(raw) + 1
        entries.append(struct.pack("<4I", path_at, len(raw), at, len(data)))
        blob += data
        at += len(data)
    if at > LIMIT:
        fail(f"the archive would be {at} bytes, more than the {LIMIT} this is meant for")
    return MAGIC + struct.pack("<2I", len(files), at) + b"".join(entries) + bytes(blob)


def unpack(archive):
    if archive[:8] != MAGIC:
        fail("not an archive of ours")
    count, size = struct.unpack_from("<2I", archive, 8)
    if size != len(archive):
        fail(f"the archive says {size} bytes and has {len(archive)}")
    out = {}
    for i in range(count):
        p, pl, d, dl = struct.unpack_from("<4I", archive, 16 + 16 * i)
        out[archive[p:p + pl].decode()] = archive[d:d + dl]
    return out


def default_persist(out):
    dump = os.path.join(out, "persist-dump", "sensors")
    return dump if os.path.isdir(dump) else None


def stage(args):
    out = os.path.abspath(args.out)
    root = os.path.join(out, "root")
    persist = args.persist or default_persist(out)
    vendor_sensors = os.path.join(args.vendor, "etc", "sensors")
    if not os.path.isfile(os.path.join(vendor_sensors, "sns_reg_config")):
        fail(f"{args.vendor}: no etc/sensors/sns_reg_config (the stock vendor partition, extracted, is what --vendor wants)")
    if os.path.isdir(root):
        shutil.rmtree(root)
    copy_tree(vendor_sensors, os.path.join(root, VENDOR_SENSORS), VENDOR_SKIP)
    if persist:
        copy_tree(persist, os.path.join(root, PERSIST_SENSORS))
        registry_source = f"persist dump {os.path.abspath(persist)} (this phone's own calibration)"
    else:
        copy_tree(args.registry, os.path.join(root, REGISTRY))
        registry_source = f"registry: {os.path.abspath(args.registry)}"
    copy_tree(args.socinfo, os.path.join(root, SOCINFO))
    archive = pack(root)
    with open(os.path.join(out, "sensors.fs"), "wb") as f:
        f.write(archive)
    files = files_of(root)
    lines = [
        "darwin-lisa: the sensors PD's files (kernel/drivers/adsp/fw-sensors-stage.py stage). Proprietary: never in git.",
        f"vendor: {os.path.abspath(args.vendor)}",
        registry_source if not persist else f"registry: none ({registry_source})",
        f"socinfo: {os.path.abspath(args.socinfo)}",
        f"sensors.fs: {len(archive)} bytes, {len(files)} files, sha256 {hashlib.sha256(archive).hexdigest()}",
    ]
    lines += [f"  {name}  {os.path.getsize(path)}" for name, path in files]
    with open(os.path.join(out, "SOURCE"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"fw-sensors-stage: {len(files)} files, archive {len(archive)} bytes in {out}")
    print(f"  {registry_source}")
    return 0 if check_dir(out) else 1


def check_dir(out):
    root = os.path.join(out, "root")
    try:
        with open(os.path.join(out, "sensors.fs"), "rb") as f:
            packed = unpack(f.read())
    except OSError as error:
        print(f"check: {error}")
        return False
    good = True
    on_disk = {}
    for name, path in files_of(root):
        with open(path, "rb") as f:
            on_disk[name] = f.read()
    if packed != on_disk:
        print("check: the archive and the tree differ: stage again")
        good = False
    for must in MUST_HAVE:
        if "/" + must not in packed:
            print(f"check: /{must} is missing")
            good = False
    configs = [n for n in packed if n.startswith("/" + VENDOR_SENSORS + "/config/")]
    registry = [n for n in packed if n.startswith("/" + REGISTRY + "/")]
    print(f"check: {len(packed)} files: {len(configs)} JSON configs, {len(registry)} registry files"
          f"{'' if registry else ' (an empty registry: the DSP builds it from the JSON, in RAM)'}")
    return good


def header(out, path):
    archive = os.path.join(os.path.abspath(out), "sensors.fs")
    if not check_dir(os.path.abspath(out)):
        raise SystemExit(1)
    if '"' in archive or "\\" in archive:
        fail(f"{archive}: a quote or backslash in the path")
    with open(archive, "rb") as f:
        data = f.read()
    text = "\n".join([
        f"/* darwin-lisa: generated by kernel/drivers/adsp/fw-sensors-stage.py header from {archive}.",
        " * Qualcomm's sensor configuration and a lisa's registry, assembled in with .incbin: never commit this. */",
        "#define DLISA_SENSORS_FS 1",
        f"#define DLISA_SENSORS_FS_SIZE {len(data)}",
        f"/* sha256 {hashlib.sha256(data).hexdigest()} */",
        "#define DLISA_SENSORS_STR2(x) #x",
        "#define DLISA_SENSORS_STR(x) DLISA_SENSORS_STR2(x)",
        "#define DLISA_SENSORS_SYM DLISA_SENSORS_STR(__USER_LABEL_PREFIX__) \"dlisa_sensors_fs\"",
        "#ifdef __APPLE__",
        r'__asm__("\t.section __TEXT,__const\n\t.globl " DLISA_SENSORS_SYM "\n\t.private_extern " DLISA_SENSORS_SYM "\n"',
        "#else",
        r'__asm__("\t.section .rodata\n\t.globl " DLISA_SENSORS_SYM "\n\t.hidden " DLISA_SENSORS_SYM "\n"',
        "#endif",
        r'	"\t.p2align 12\n" DLISA_SENSORS_SYM ":\n"',
        "\t" + r'"\t.incbin \"' + archive + r'\"\n"',
        "\t" + r'"\t.text\n");',
        'extern const uint8_t dlisa_sensors_fs[] __attribute__((visibility("hidden")));',
        "",
    ])
    if os.path.exists(path) and open(path).read() == text:
        print(f"{path}: the same already")
        return 0
    with open(path, "w") as f:
        f.write(text)
    print(f"{path}: {len(data)} bytes of sensor files built in")
    return 0


REMOTE_TAR = "/tmp/dlisa-persist-sensors.tar"
REMOTE = r"""set -eu
dev=/dev/disk/by-partlabel/persist
[ -b "$dev" ] || { echo "no $dev" >&2; exit 3; }
at=$(findmnt -n -o TARGET -S "$dev" | head -n 1 || true)
if [ -n "$at" ]; then
    tar -C "$at" -cf TAR sensors
else
    d=$(mktemp -d)
    mount -o ro,noload "$dev" "$d"
    tar -C "$d" -cf TAR sensors || { umount "$d"; rmdir "$d"; exit 4; }
    umount "$d"
    rmdir "$d"
fi
chmod 644 TAR
""".replace("TAR", REMOTE_TAR)


def dump_persist(args):
    """Read-only: the remote side mounts persist ro,noload (or reads it where pmOS mounted it) and runs tar into /tmp.
    The tar comes back by scp (a pty, which sudo's password needs, would mangle a binary stream)."""
    out = os.path.join(os.path.abspath(args.out), "persist-dump")
    local = os.path.join(os.path.abspath(args.out), "persist-sensors.tar")
    print(f"fw-sensors-stage: reading persist's sensors/ on {args.host} (sudo asks for the password; read-only mount)")
    if subprocess.run(["ssh", "-t", args.host, "sudo sh -c " + shlex_quote(REMOTE)]).returncode != 0:
        fail("the remote side failed (see above)")
    os.makedirs(os.path.dirname(local), exist_ok=True)
    if subprocess.run(["scp", f"{args.host}:{REMOTE_TAR}", local]).returncode != 0:
        fail("scp failed")
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    with tarfile.open(local) as tar:
        tar.extractall(out, filter="data")
    count = sum(len(names) for _, _, names in os.walk(out))
    print(f"fw-sensors-stage: {count} files in {out}/sensors; `stage` picks the dump up from there")
    return 0


def shlex_quote(text):
    return "'" + text.replace("'", "'\"'\"'") + "'"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("stage")
    for name in ("vendor", "registry", "socinfo", "out"):
        s.add_argument("--" + name, default=DEFAULTS[name])
    s.add_argument("--persist", default=None)
    c = sub.add_parser("check")
    c.add_argument("out", nargs="?", default=DEFAULTS["out"])
    h = sub.add_parser("header")
    h.add_argument("out")
    h.add_argument("header")
    d = sub.add_parser("dump-persist")
    d.add_argument("--host", default="mateus@172.16.42.1")
    d.add_argument("--out", default=DEFAULTS["out"])
    args = parser.parse_args(argv[1:])
    if args.command == "stage":
        return stage(args)
    if args.command == "check":
        return 0 if check_dir(os.path.abspath(args.out)) else 1
    if args.command == "header":
        return header(args.out, args.header)
    return dump_persist(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
