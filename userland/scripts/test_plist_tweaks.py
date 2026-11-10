#!/usr/bin/env python3
"""Tests of plist-tweaks.py's sysroot additions: python3 -I userland/scripts/test_plist_tweaks.py"""
import importlib.util
import os
import plistlib
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("plist_tweaks", os.path.join(HERE, "plist-tweaks.py"))
tweaks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tweaks)


def writePlist(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        plistlib.dump(value, f, fmt=plistlib.FMT_BINARY)


class TimingProfileTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.profiles = os.path.join(self.root, "System/Library/EventTimingProfiles")
        writePlist(os.path.join(self.profiles, "J817.Default.plist"), {})
        writePlist(os.path.join(self.profiles, "J817.Touch.plist"), {"Profiles": {"Touch": {"Period": 0.004}}})

    def testDefaultProfileIsTheEmptyOneUnderTheBoardName(self):
        made = tweaks.saveDefaultTimingProfile(self.root, "D22")
        self.assertEqual(made, os.path.join(self.profiles, "D22.Default.plist"))
        with open(made, "rb") as f:
            self.assertEqual(plistlib.load(f), {})

    def testNoTouchProfileForTheBoard(self):
        tweaks.saveDefaultTimingProfile(self.root, "D22")
        self.assertFalse(os.path.exists(os.path.join(self.profiles, "D22.Touch.plist")))

    def testRunTwiceIsTheSame(self):
        with open(tweaks.saveDefaultTimingProfile(self.root, "D22"), "rb") as f:
            first = f.read()
        with open(tweaks.saveDefaultTimingProfile(self.root, "D22"), "rb") as f:
            second = f.read()
        self.assertEqual(first, second)


class LdGroupsTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        daemons = os.path.join(self.root, "System/Library/LaunchDaemons")
        for name, label in [("com.apple.a.plist", "com.apple.a"), ("com.apple.b-noncellular.plist", "com.apple.b"),
                            ("com.apple.ClarityBoard.plist", "com.apple.ClarityBoard"),
                            ("com.apple.c.plist", "com.apple.c")]:
            writePlist(os.path.join(daemons, name), {"Label": label})
        self.listing = os.path.join(self.root, "lean.txt")
        with open(self.listing, "w") as f:
            f.write("# comment\ncom.apple.a.plist\ncom.apple.b-noncellular.plist\n\ncom.apple.ClarityBoard.plist\n"
                    "com.apple.c.plist\ncom.apple.missing.plist\n")

    def groups(self, size):
        path = tweaks.saveLdGroups(self.root, self.listing, size)
        with open(path) as f:
            return {line.split()[0]: line.split()[1:] for line in f if line.strip() and not line.startswith("#")}

    def testLeanHasEveryLabelOnceFromTheDaemonsPlist(self):
        lean = self.groups(2)["lean"]
        self.assertEqual(lean, ["com.apple.a", "com.apple.b", "com.apple.ClarityBoard", "com.apple.c"])

    def testChunksCoverLeanInOrder(self):
        groups = self.groups(3)
        self.assertEqual(groups["lean1"], ["com.apple.a", "com.apple.b", "com.apple.ClarityBoard"])
        self.assertEqual(groups["lean2"], ["com.apple.c"])
        self.assertNotIn("lean3", groups)

    def testUiShellsGroupHasOnlyThoseInTheList(self):
        self.assertEqual(self.groups(3)["ui"], ["com.apple.ClarityBoard"])

    def testDefaultGroupHasTheAlwaysDisabledLabels(self):
        path = tweaks.saveLdGroups(self.root, self.listing, 3, alwaysDisabled=("com.apple.c", "com.apple.missing"))
        with open(path) as f:
            groups = {line.split()[0]: line.split()[1:] for line in f if line.strip() and not line.startswith("#")}
        self.assertEqual(groups["default"], ["com.apple.c"])

    def testFileGoesToPrivateEtc(self):
        path = tweaks.saveLdGroups(self.root, self.listing, 3)
        self.assertEqual(path, os.path.join(self.root, "private/etc/dlisa-ld-groups.conf"))


if __name__ == "__main__":
    unittest.main()
