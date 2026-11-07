import struct
import unittest

from dyldcache import is_cache, strip_cache
from test_macho import BRAA_X16_X17, BR_X16, LDRAB_OUT_OF_RANGE, RET, RETAB, words_at

VM_PROT_READ, VM_PROT_WRITE, VM_PROT_EXECUTE = 1, 2, 4
MAPPING_OFFSET = 0x200


def cache(mappings, magic=b"dyld_v1  arm64e"):
    """A cache file: the header's mapping table at 0x200, then each mapping's
    words at its file offset. mappings: [(file_offset, init_prot, words)]."""
    size = max(off + 4 * len(words) for off, _, words in mappings)
    data = bytearray(size)
    data[0:16] = magic.ljust(16, b"\0")
    struct.pack_into("<II", data, 16, MAPPING_OFFSET, len(mappings))
    for i, (off, prot, words) in enumerate(mappings):
        struct.pack_into("<QQQII", data, MAPPING_OFFSET + 32 * i, 0x180000000 + off, 4 * len(words), off, prot, prot)
        struct.pack_into(f"<{len(words)}I", data, off, *words)
    return data


class DyldCache(unittest.TestCase):
    def test_rewrites_executable_mappings(self):
        data = cache([(0x1000, VM_PROT_READ | VM_PROT_EXECUTE, [RETAB, BRAA_X16_X17])])
        report = strip_cache(data)
        self.assertEqual(words_at(data, 0x1000, 2), [RET, BR_X16])
        self.assertEqual(report.patched, 2)

    def test_leaves_data_mappings_alone(self):
        data = cache([(0x1000, VM_PROT_READ | VM_PROT_EXECUTE, [RETAB]),
                      (0x2000, VM_PROT_READ | VM_PROT_WRITE, [RETAB])])
        strip_cache(data)
        self.assertEqual(words_at(data, 0x2000, 1), [RETAB])

    def test_reports_traps_by_file_offset(self):
        data = cache([(0x1000, VM_PROT_READ | VM_PROT_EXECUTE, [RET, LDRAB_OUT_OF_RANGE])])
        self.assertEqual(strip_cache(data).traps, [(0x1004, LDRAB_OUT_OF_RANGE)])

    def test_recognises_the_magic(self):
        self.assertTrue(is_cache(cache([(0x1000, VM_PROT_READ, [0])])))
        self.assertFalse(is_cache(bytearray(b"\xcf\xfa\xed\xfe" + b"\0" * 60)))

    def test_rejects_a_cache_for_another_architecture(self):
        with self.assertRaises(ValueError):
            strip_cache(cache([(0x1000, VM_PROT_EXECUTE, [RETAB])], magic=b"dyld_v1  x86_64"))


if __name__ == "__main__":
    unittest.main()
