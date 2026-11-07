"""Strip pointer authentication from a dyld shared cache file, in place.

The main cache and each sub-cache (.01, .02, ...) are files of their own,
each with a header and a mapping table; every mapping whose initial
protection includes execute is rewritten word by word.

Not yet: the data-in-code ranges of the images inside the cache, and the
CodeDirectory page hashes (the kernel's pager path does not need either
unless the scan finds data-in-code in the cache's text).
"""
import struct

from macho import DataRanges, Report, strip_section

MAGIC_PREFIX = b"dyld_v1 "
ARM64_MAGICS = (b"dyld_v1  arm64e", b"dyld_v1   arm64")
VM_PROT_EXECUTE = 4


def is_cache(data):
    return bytes(data[:8]) == MAGIC_PREFIX


def mappings(data):
    """(file_offset, size, init_prot) of every mapping."""
    offset, count = struct.unpack_from("<II", data, 16)
    for i in range(count):
        _address, size, file_offset, _max_prot, init_prot = struct.unpack_from("<QQQII", data, offset + 32 * i)
        yield file_offset, size, init_prot


def strip_cache(data):
    magic = bytes(data[:16]).rstrip(b"\0")
    if magic not in ARM64_MAGICS:
        raise ValueError(f"not an arm64 dyld shared cache: {magic!r}")
    report = Report()
    for file_offset, size, prot in mappings(data):
        if prot & VM_PROT_EXECUTE:
            report.add(strip_section(data, file_offset, size, DataRanges([])))
    return report
