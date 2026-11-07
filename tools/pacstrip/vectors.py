#!/usr/bin/env python3
"""Write test vectors for other implementations of insn.rewrite().

One line per word: "<word> <status> <result>", hex words, status 0 keep,
1 patch, 2 trap. Every encoding of the rewritten families (the ones the
exhaustive tests check against llvm-mc) and a fixed sample of random words.

usage: vectors.py OUT [--random N]
"""
import argparse
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from insn import KEEP, PATCH, TRAP, rewrite  # noqa: E402

STATUS = {KEEP: 0, PATCH: 1, TRAP: 2}


def families():
    for base in (0xD71F0800, 0xD73F0800):  # BRAA/BRAB, BLRAA/BLRAB
        yield from (base | m << 10 | n << 5 | r for m in (0, 1) for n in range(32) for r in range(32))
    for base in (0xD61F081F, 0xD63F081F):  # BRAAZ/BRABZ, BLRAAZ/BLRABZ
        yield from (base | m << 10 | n << 5 for m in (0, 1) for n in range(32))
    yield from (0xDAC10000 | op << 10 | n << 5 | d for op in range(64) for n in range(32) for d in (0, 16, 31))
    yield from (0x9AC03000 | m << 16 | n << 5 | d for m in range(32) for n in (0, 1, 31) for d in (0, 31))
    yield from (0xF8200400 | m << 23 | s << 22 | imm << 12 | w << 11 | n << 5 | t
                for m in (0, 1) for s in (0, 1) for imm in range(512) for w in (0, 1) for n, t in ((6, 5), (31, 0)))
    yield from (0xD65F0BFF, 0xD65F0FFF, 0xD69F0BFF, 0xD69F0FFF, 0x1E7E0020, 0xD503233F, 0xD503201F)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--random", type=int, default=1_000_000)
    args = ap.parse_args()
    rng = random.Random(0x1154)  # fixed: the same vectors on every run
    words = list(families()) + [rng.getrandbits(32) for _ in range(args.random)]
    with args.out.open("w") as out:
        for word in words:
            status, result = rewrite(word)
            out.write(f"{word:08x} {STATUS[status]} {result:08x}\n")


if __name__ == "__main__":
    main()
