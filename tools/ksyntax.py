#!/usr/bin/env python3
"""ksyntax.py REF.o.json SOURCE [--include-first DIR]... : a syntax-only compile of SOURCE with the exact flags the
26 kernel build used for REF (its compile record next to the object: bsd/DEVELOPMENT/dlisa.battery.o.json for a C
file of bsd/dev, iokit/DEVELOPMENT/IOPlatformExpert.cpo.json for C++ in iokit/Kernel), in the toolchain image.

Nothing is built or written: the build tree is mounted read only and clang runs with -fsyntax-only. --include-first DIR
puts DIR in front of the include path (a copy of the tree's bsd/ with new headers in it). Exit status: clang's.
"""
import argparse
import json
import os
import subprocess
import sys

MOBILE = "/home/mateusseiboth/Dev/mobile"
OBJ = MOBILE + "/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
IMAGE = "darwin-lisa/unixnu-toolchain:latest"


def read_record(name):
    with open(name) as f:
        record, _ = json.JSONDecoder().raw_decode(f.read())
    return record


def build_arguments(record, source, first):
    old = record["arguments"]
    out = [old[0]]
    skip = False
    for arg in old[1:]:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if arg == "-c":
            continue
        out.append(source if arg == record["file"] else arg)
    out.insert(2, "-fsyntax-only")
    # a copy kept out of the tree still finds the "quoted" headers that sit beside the original
    out[3:3] = ["-iquote", os.path.dirname(record["file"])]
    for directory in reversed(first):
        out[3:3] = ["-I", directory]
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("record")
    parser.add_argument("source")
    parser.add_argument("--include-first", action="append", default=[])
    options = parser.parse_args()
    record = read_record(options.record if os.path.isabs(options.record) else os.path.join(OBJ, options.record))
    source = os.path.abspath(options.source)
    arguments = build_arguments(record, source, [os.path.abspath(d) for d in options.include_first])
    mounts = ["-v", f"{MOBILE}:{MOBILE}:ro"]
    for extra in {os.path.dirname(source), *options.include_first}:
        extra = os.path.abspath(extra)
        if not extra.startswith(MOBILE + "/"):
            mounts += ["-v", f"{extra}:{extra}:ro"]
    command = ["docker", "run", "--rm", *mounts, "-w", record["directory"], IMAGE, *arguments]
    result = subprocess.run(command, capture_output=True, text=True)
    lines = result.stderr.splitlines()
    errors = [line for line in lines if "error:" in line]
    # the tree's own headers warn under -Weverything for every file (-Wshadow-header...): only SOURCE's are said
    own = [line for line in lines if line.startswith(source + ":") and ": warning:" in line and "-Wshadow-header" not in line]
    for line in errors + own:
        print(line)
    print(f"ksyntax: {os.path.basename(source)}: exit {result.returncode}, {len(errors)} errors, "
          f"{len(own)} warnings in the file itself")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
