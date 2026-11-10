"""darwin-lisa: what the wire scripts share (rtc/rtc-wire.py, wifi/wifi-join-wire.py).

Every edit is idempotent: it says [skip] when its text is already there and [FAIL] (exit 2) when its anchor is not
there exactly once. XNU_WORK=<dir> points the scripts at another tree (a dry run on a copy).
"""
import os
import re
import shutil
import sys

TREE = os.environ.get(
    "XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")


def path(relative):
    return os.path.join(TREE, relative)


def read(name):
    with open(name) as f:
        return f.read()


def write(name, text):
    with open(name, "w") as f:
        f.write(text)


def fail(tag, why):
    print(f"  [FAIL] {tag}: {why}")
    sys.exit(2)


def copy(source, relative):
    shutil.copyfile(source, path(relative))
    print(f"  [ok]   copied {os.path.basename(source)} -> {relative}")


def edit(relative, old, new, tag):
    name = path(relative)
    text = read(name)
    if new in text:
        print(f"  [skip] {tag}: already applied")
        return
    count = text.count(old)
    if count != 1:
        fail(tag, f"anchor found {count} times (need 1) in {relative}")
    write(name, text.replace(old, new))
    print(f"  [ok]   {tag}")


def insert_after_line(relative, anchor, line, tag):
    """`line` (with its newline) right after the one line that contains `anchor`."""
    name = path(relative)
    text = read(name)
    if line in text:
        print(f"  [skip] {tag}: already applied")
        return
    lines = text.splitlines(keepends=True)
    hits = [i for i, each in enumerate(lines) if anchor in each]
    if len(hits) != 1:
        fail(tag, f"anchor {anchor!r} on {len(hits)} lines (need 1) in {relative}")
    lines.insert(hits[0] + 1, line)
    write(name, "".join(lines))
    print(f"  [ok]   {tag}")


def append_once(relative, marker, text, tag):
    name = path(relative)
    if marker in read(name):
        print(f"  [skip] {tag}: already applied")
        return
    with open(name, "a") as f:
        f.write(text)
    print(f"  [ok]   {tag}")


def add_bsd_dev_tag(tag_name):
    """The driver tag on the BSD_DEV line of config/MASTER.arm64.bcm2837, right before BSD_EXTRAS_DEV."""
    relative = "config/MASTER.arm64.bcm2837"
    name = path(relative)
    text = read(name)
    line = re.search(r"^#  BSD_DEV =.*$", text, re.M)
    if line is None or "BSD_EXTRAS_DEV ]" not in line.group(0):
        fail(f"MASTER.arm64.bcm2837 += {tag_name}", "no BSD_DEV line ending in BSD_EXTRAS_DEV ]")
    if f" {tag_name} " in line.group(0):
        print(f"  [skip] MASTER.arm64.bcm2837 += {tag_name}: already applied")
        return
    changed = line.group(0).replace("BSD_EXTRAS_DEV ]", f"{tag_name} BSD_EXTRAS_DEV ]")
    write(name, text[:line.start()] + changed + text[line.end():])
    print(f"  [ok]   MASTER.arm64.bcm2837 += {tag_name}")


def add_option(option, comment, tag_name):
    """options OPTION in config/MASTER, after the battery's line (every port option sits in that block)."""
    insert_after_line("config/MASTER", "HAKIOS_BATTERY\t#",
                      f"options\t\t{option}\t# {comment}\t# <{tag_name}>\n", f"config/MASTER {option}")


def require(relative, needle, why):
    name = path(relative)
    if not os.path.exists(name) or needle not in read(name):
        fail(why, f"{needle!r} not in {relative}")
