#!/usr/bin/env python3
"""List the instructions of a Mach-O that one CPU decodes and another does not.

LLVM's disassembler is the oracle: the file is disassembled for the CPU the
code was built for (`want`, by default the Apple A13, the oldest iOS 26 CPU)
and for the CPU it has to run on (`have`, by default the Cortex-A78 of the
SM7325). An instruction that decodes differently for `have` (as <unknown>, or as a
generic system register access, the way CFINV does without FlagM) must be
rewritten or emulated.

usage: featscan.py FILE [--have CPU] [--want CPU]
"""
import argparse
import collections
import re
import shutil
import subprocess
from pathlib import Path

# the raw encoding is one word (Mach-O) or four bytes (other formats)
LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{8}|(?:[0-9a-f]{2} ){3}[0-9a-f]{2})\s+(\S+)")
SECTION = re.compile(r"^Disassembly of section (.+):$")
UNKNOWN = "<unknown>"
# Apple syntax puts the vector arrangement on the mnemonic: fcmla.4s
ARRANGEMENT = re.compile(r"\.\d+[bhsdq]$")


def disassemble(path, cpu):
    """{(section, address): mnemonic} for every word of the code sections."""
    objdump = shutil.which("llvm-objdump") or "llvm-objdump"
    out = subprocess.run([objdump, "-d", f"--mcpu={cpu}", str(path)], capture_output=True, text=True, check=True)
    words, section = {}, ""
    for line in out.stdout.splitlines():
        header = SECTION.match(line)
        section = header.group(1) if header else section
        insn = LINE.match(line)
        if insn:
            words[section, int(insn.group(1), 16)] = ARRANGEMENT.sub("", insn.group(2))
    return words


def scan(path, have="cortex-a78", want="apple-a13"):
    """{mnemonic: count} of what `want` decodes and `have` decodes otherwise."""
    target, source = disassemble(path, have), disassemble(path, want)
    missing = (source[key] for key, mnemonic in target.items()
               if source.get(key, UNKNOWN) not in (UNKNOWN, mnemonic))
    return dict(collections.Counter(missing))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--have", default="cortex-a78")
    ap.add_argument("--want", default="apple-a13")
    args = ap.parse_args()
    for mnemonic, count in sorted(scan(args.file, args.have, args.want).items(), key=lambda kv: -kv[1]):
        print(f"{count:8d} {mnemonic}")


if __name__ == "__main__":
    main()
