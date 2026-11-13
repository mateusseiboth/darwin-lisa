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

    def testLoopsGroupHasTheCrashLoopingLabelsPresent(self):
        path = tweaks.saveLdGroups(self.root, self.listing, 3, crashLooping=("com.apple.a", "com.apple.missing"))
        with open(path) as f:
            groups = {line.split()[0]: line.split()[1:] for line in f if line.strip() and not line.startswith("#")}
        self.assertEqual(groups["loops"], ["com.apple.a"])

    def testLoopsGroupIsNotDisabledByDefault(self):
        path = tweaks.saveLdGroups(self.root, self.listing, 3, alwaysDisabled=("com.apple.c",),
                                   crashLooping=("com.apple.a",))
        with open(path) as f:
            groups = {line.split()[0]: line.split()[1:] for line in f if line.strip() and not line.startswith("#")}
        self.assertNotIn("com.apple.a", groups["default"])

    def testCrashLoopingDefaultsAreTheThreeSeenInEveryRun(self):
        self.assertEqual(tweaks.CRASH_LOOPING,
                         ("com.apple.wifip2pd", "com.apple.syncdefaultsd", "com.apple.security.cryptexd"))

    def testFileGoesToPrivateEtc(self):
        path = tweaks.saveLdGroups(self.root, self.listing, 3)
        self.assertEqual(path, os.path.join(self.root, "private/etc/dlisa-ld-groups.conf"))


class IphoneWallpaperLookupsTest(unittest.TestCase):
    """The 23F84 iPad image ships only Collections~ipad.plist and DefaultWallpapers~ipad.plist; WallpaperKit on a phone
    reads Collections~iphone.plist (else Collections.plist) and DefaultWallpapers~iphone.plist."""

    STRIPE = "2939274C-F444-4AA8-806E-6E3FAFBA79B1"
    BOKEH = "D4894D14-DB0F-4D04-AB02-07C39C5E5466"
    LAKE = "B18891A2-4DB9-43A8-8C97-39C9CC7322AE"
    IPADOS = "62879BFA-F700-4384-83E1-7AD1CC0D081E"

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.wallpaper = os.path.join(self.root, "Library/Wallpaper")
        self.collections = os.path.join(self.wallpaper, "Collections")
        writePlist(os.path.join(self.collections, "Collections~ipad.plist"),
                   {"order": [self.IPADOS, "A8434BA6-DDDE-4DCC-9F9A-21A9F8013B8F", self.LAKE, self.STRIPE, self.BOKEH]})
        self.addCollection("iPadOS_17~ipad", {"identifier": self.IPADOS, "order": [7570]})
        self.addCollection("Lake~ipad", {"identifier": self.LAKE, "order": [7560]})
        self.addCollection("Stripe", {"identifier": self.STRIPE, "order": [7365, 7370]})
        self.addCollection("Bokeh", {"identifier": self.BOKEH, "order": [5010, 5020],
                                     "supportedDeviceClasses": ["iPhone"]})
        self.addCollection("PadOnly", {"identifier": "11111111-0000-0000-0000-000000000000", "order": [1],
                                       "supportedDeviceClasses": ["iPad"]})

    def addCollection(self, name, metadata):
        writePlist(os.path.join(self.collections, f"{name}.wallpaperCollection/WallpaperCollection.plist"), metadata)

    def read(self, relative):
        with open(os.path.join(self.wallpaper, relative), "rb") as f:
            return plistlib.load(f)

    def testOrderHasThePhoneCollectionsInTheIpadOrder(self):
        tweaks.saveIphoneWallpaperLookups(self.root)
        self.assertEqual(self.read("Collections/Collections~iphone.plist"), {"order": [self.STRIPE, self.BOKEH]})

    def testPlainCollectionsPlistIsTheSame(self):
        tweaks.saveIphoneWallpaperLookups(self.root)
        self.assertEqual(self.read("Collections/Collections.plist"), self.read("Collections/Collections~iphone.plist"))

    def testDefaultIsTheFirstWallpaperOfTheFirstCollection(self):
        tweaks.saveIphoneWallpaperLookups(self.root)
        self.assertEqual(self.read("DefaultWallpapers~iphone.plist"),
                         {"default": {"collectionIdentifier": self.STRIPE, "wallpaperIdentifier": 7365}})

    def testIpadFilesAreLeftAlone(self):
        tweaks.saveIphoneWallpaperLookups(self.root)
        self.assertEqual(self.read("Collections/Collections~ipad.plist")["order"][0], self.IPADOS)

    def testRunTwiceIsTheSame(self):
        tweaks.saveIphoneWallpaperLookups(self.root)
        first = self.read("Collections/Collections.plist")
        tweaks.saveIphoneWallpaperLookups(self.root)
        self.assertEqual(self.read("Collections/Collections.plist"), first)

    def testNoWallpaperFolderMakesNothing(self):
        empty = tempfile.mkdtemp()
        self.assertEqual(tweaks.saveIphoneWallpaperLookups(empty), [])
        self.assertFalse(os.path.exists(os.path.join(empty, "Library/Wallpaper")))


if __name__ == "__main__":
    unittest.main()
