#!/usr/bin/env python3
"""Strip pointer authentication from arm64e Mach-O files.

usage:
  pacstrip.py scan FILE...        count what would change; list what the kernel must emulate
  pacstrip.py strip IN [OUT]      rewrite IN into OUT (in place without OUT)
"""
import argparse
import sys
from pathlib import Path

# run with python3 -I as well: the modules live next to this file
sys.path.insert(0, str(Path(__file__).resolve().parent))

from macho import strip  # noqa: E402


def describe(path, report):
    lines = [f"{path}: {report.patched} to patch, {len(report.traps)} to emulate"]
    lines += [f"  {offset:#x} {word:#010x}" for offset, word in report.traps]
    return "\n".join(lines)


def scan(args):
    for path in args.files:
        print(describe(path, strip(bytearray(path.read_bytes()))))


def rewrite(args):
    data = bytearray(args.input.read_bytes())
    report = strip(data)
    (args.output or args.input).write_bytes(data)
    print(describe(args.input, report))


def parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = ap.add_subparsers(dest="command", required=True)
    scan_cmd = commands.add_parser("scan")
    scan_cmd.add_argument("files", nargs="+", type=Path)
    scan_cmd.set_defaults(run=scan)
    strip_cmd = commands.add_parser("strip")
    strip_cmd.add_argument("input", type=Path)
    strip_cmd.add_argument("output", nargs="?", type=Path)
    strip_cmd.set_defaults(run=rewrite)
    return ap


def main():
    args = parser().parse_args()
    try:
        args.run(args)
    except ValueError as error:
        sys.exit(f"pacstrip: {error}")


if __name__ == "__main__":
    main()
