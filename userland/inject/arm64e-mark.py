#!/usr/bin/env python3
"""arm64e-mark.py FILE: mark a linked arm64 Mach-O as arm64e (cpusubtype 0x80000002, what iOS 26's own binaries
carry) and re-hash its ad-hoc code signature's page slots, which cover the header.

lld 18 writes cpusubtype ALL for -arch arm64e, and dyld inserts a library into an arm64e process only when the
library is arm64e too. The code itself has no pointer authentication in it, so only the label changes."""
import hashlib
import struct
import sys

LC_CODE_SIGNATURE = 0x1D
CSMAGIC_CODEDIRECTORY = 0xFADE0C02
ARM64E_PTRAUTH = 0x80000002
HASHES = {1: hashlib.sha1, 2: hashlib.sha256}


def findSignature(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, at)
        if cmd == LC_CODE_SIGNATURE:
            return struct.unpack_from("<II", data, at + 8)
        at += size
    sys.exit("arm64e-mark: no LC_CODE_SIGNATURE (link with -adhoc_codesign)")


def rehashDirectory(data, cd):
    version, flags, hashOffset, identOffset, nSpecial, nCode, codeLimit, hashSize, hashType, _, pageShift = \
        struct.unpack_from(">IIIIIIIBBBB", data, cd + 8)
    page = 1 << pageShift
    algorithm = HASHES[hashType]
    for slot in range(nCode):
        start = slot * page
        digest = algorithm(bytes(data[start:min(start + page, codeLimit)])).digest()[:hashSize]
        data[cd + hashOffset + slot * hashSize: cd + hashOffset + (slot + 1) * hashSize] = digest
    return nCode


def main(path):
    data = bytearray(open(path, "rb").read())
    if struct.unpack_from("<I", data, 0)[0] != 0xFEEDFACF:
        sys.exit("arm64e-mark: not a thin 64-bit Mach-O")
    struct.pack_into("<I", data, 8, ARM64E_PTRAUTH)
    sigOffset, _ = findSignature(data)
    magic, _, count = struct.unpack_from(">III", data, sigOffset)
    slots = 0
    for i in range(count):
        _, offset = struct.unpack_from(">II", data, sigOffset + 12 + i * 8)
        if struct.unpack_from(">I", data, sigOffset + offset)[0] == CSMAGIC_CODEDIRECTORY:
            slots += rehashDirectory(data, sigOffset + offset)
    open(path, "wb").write(data)
    print(f"arm64e-mark: {path}: cpusubtype {ARM64E_PTRAUTH:#x}, {slots} page hashes rewritten")


main(sys.argv[1])
