"""Host tests of select-system-26.py's choice of programs: python3 -m unittest test_select_system (in this directory)."""
import importlib.util
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location("select_system", os.path.join(HERE, "select-system-26.py"))
SELECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SELECT)


class BigPrograms(unittest.TestCase):
    def test_bluetoothd_is_kept_although_big(self):
        self.assertTrue(SELECT.wanted_file("usr/sbin/bluetoothd", 12151072))

    def test_other_big_daemons_stay_out(self):
        self.assertFalse(SELECT.wanted_file("usr/libexec/locationd", 40226848))
        self.assertFalse(SELECT.wanted_file("usr/libexec/sharingd", 11567744))

    def test_small_daemons_are_taken(self):
        self.assertTrue(SELECT.wanted_file("usr/sbin/BlueTool", 5242880))


if __name__ == "__main__":
    unittest.main()
