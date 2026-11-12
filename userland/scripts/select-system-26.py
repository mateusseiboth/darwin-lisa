#!/usr/bin/env python3
"""Choose the part of the iOS 26 (23F84) root filesystem that goes into the in-memory system image.

Adapted from HakiOS sysimage/select-system.py (iOS 16). The selection logic is
version-agnostic (it keeps whole programs/libraries/data outside the framework
dirs, and only the small run-time resources from inside them), and the iOS-26
root keeps the same shape it had on iOS 16: System/Library/Frameworks (281 of
them, resource bundles whose code is in the dyld shared cache), usr/, Applications/,
Library/, and the applications waiting in private/var/staged_system_apps.

What is NEW on iOS 26 is handled by the build script, not here:
  - the filesystem dmg is AEA-encrypted (094-56106-108.dmg.aea): it is decrypted
    with `ipsw` before `7z l -slt` can list it (build-system-26.sh);
  - /System/Cryptexes/{OS,App,ExclaveOS,Supplemental} are SYMLINKS into
    /private/preboot/Cryptexes/..., so the OS/App cryptex content (and the dyld
    shared cache) is laid at the symlink target, not under /System/Cryptexes.
This script still only picks regular files/dirs/symlinks OUT OF THE ROOTFS dmg.

usage: select-system-26.py rootfs.slt system.files system.manifest

rootfs.slt is the technical listing of the decrypted root filesystem image ("7z l -slt rootfs.dmg").
Two files are written:
  system.files      the regular files to extract ("7z x -orootfs rootfs.dmg @system.files")
  system.manifest   every chosen file, directory and symbolic link with its owner, group and mode,
                    tab separated: mode(octal, with the type bits) uid gid path [link target].
                    tools/apfs/mkapfs.py builds the image from the extracted files and this manifest,
                    because extraction on Linux keeps neither owners nor all of the symlinks.

The whole root is 7.9 GB and the kernel on the phone has about 4 GB of RAM for everything, so:
  - programs, libraries and their data outside the framework directories are taken whole;
  - from the framework directories (the code itself is in the dyld shared cache) what a
    program may open at run time and is not huge: property lists, string tables, resources,
    plug-ins, and the XPC services and helper executables bundled with frameworks;
  - the applications (the home screen is made of their icons), without their media; those that Darwin
    keeps in /private/var/staged_system_apps until its first boot installs them (Maps, Notes,
    Weather, ...) are put straight into /Applications, where they are system applications like
    the rest: system.files names them where they are in the image, system.manifest where they go
    (userland/build.sh moves the extracted files);
  - machine-learning models, other languages' localizations, media and GPU shader libraries
    are left out, and so are a few very big daemons that have nothing to do with the display;
  - /Library (the wallpapers, regional tables) without its sounds;
  - of pictures, what the user interface itself is drawn with: small images, asset catalogs of
    moderate size; the fonts, the two biggest (colour emoji and Chinese) made smaller by unpack.sh;
  - the empty LANGUAGE.lproj directories of the bundles whose strings are in one .loctable file
    for all languages: they are how CoreFoundation knows which languages a bundle has, and
    without them every string comes out as its key.
"""
import os
import re
import stat
import sys

# APPLE_ONLY=keep in the environment: nothing is left out for being Apple-hardware-only (the Neural Engine models, the
# ISP and display tables, the Apple GPU driver bundle, the camera effects, the camera pipeline's shader libraries): the
# fully complete image, about 370 MB more, for a root region grown by docs/data-volume.md's plan B. Without it those go,
# to make room for the shader libraries in today's 4,831,838,208-byte region (2026-11-11). prune-sysroot.py applies the
# same choice to an existing sysroot, and restores it.
KEEP_APPLE_ONLY = os.environ.get('APPLE_ONLY') == 'keep'

WHOLE =('usr/libexec/', 'usr/sbin/', 'usr/bin/', 'usr/lib/', 'usr/share/', 'sbin/', 'bin/', 'private/etc/',
         'System/Library/', 'Applications/', 'Library/')
# ... except these parts of /System/Library: big, and not needed to get a picture on the screen
# (not OnBoardingBundles, the privacy notices: an application's first screen links to its notice, and
# News refuses to start without its own -- "a legal requirement"; nor Health, the plug-ins the Health
# application and its daemon are made of: both stop with a fatal error when the directory is not there.
# Of Health only the pictures and films of the activity awards are left out, see SKIP_PREFIX and HEALTH)
SKIP_SYSTEM_LIBRARY = ('LinguisticData', 'PreinstalledAssetsV2', 'NanoTimeKit', 'Audio',
                       'Assistant', 'AccessibilityBundles',
                       'VideoProcessors', 'ScreenReader', 'Photos', 'Carrier Bundles', 'NanoPreferenceBundles',
                       'ImagingNetworks', 'ProductDocuments', 'AppPlaceholders', 'UserNotifications',
                       'Caches')
# Apple-hardware-only payload (KEEP_APPLE_ONLY): the iPad's hardware tables, the Apple ISP's neural data
# (System/Library/Isp, 52 MB) and the J817/J820 panels' overdrive and colour tables (System/Library/Display, 35 MB);
# the Apple GPU driver bundle (its ds.g17g, 29 MB, and its own shader libraries) and the camera effects' 2,200 template
# pictures (50 MB; no camera on this port). Dropped with the Neural Engine models (.hwx, SKIP_EXT) to make room for the
# shader libraries (2026-11-11).
APPLE_ONLY_SYSTEM_LIBRARY = () if KEEP_APPLE_ONLY else ('Isp', 'Display')
SKIP_APPLE_ONLY = () if KEEP_APPLE_ONLY else ('System/Library/Extensions/AGXMetalG17G.bundle/',
                                             'System/Library/PrivateFrameworks/CameraEffectsKit.framework/')
SKIP_SYSTEM_LIBRARY += APPLE_ONLY_SYSTEM_LIBRARY
FRAMEWORKS = ('System/Library/Frameworks/', 'System/Library/PrivateFrameworks/')
SMALL = 4 << 20
# never from frameworks: machine-learning models, sound and video, big tables.
# (Not the data models, .mom: an application that keeps its data with Core Data dies without its
# model -- Notes did; nor the animations and vector pictures the interface is drawn with.)
SKIP_EXT = ('.weights', '.weights_nonane', '.mov', '.pcm', '.scnz', '.espresso.net',
            '.espresso.shape', '.caf', '.m4a', '.aiff', '.mp4', '.heic', '.wav', '.art', '.scube',
            '.ds', '.dat', '.bin')
# Neural Engine programs (Vision, FRC, TextRecognition, AppleCVA...: 150 MB in all), each under 4 MB, so they slipped
# past SMALL; there is no Neural Engine here to run one (KEEP_APPLE_ONLY keeps them)
SKIP_EXT += () if KEEP_APPLE_ONLY else ('.hwx',)
# shader libraries: every one, whatever its size (MetalPerformanceShaders' are 3-72 MB). They were left out while only
# backboardd had a Metal device; with the plug-in in every process (metal_all=1) a framework loads its own library as
# soon as it has a device, and MPS aborts the process when its sub-framework's default.metallib is missing
# (2026-11-11: SpringBoard crash-looped through SiriUICore's blur). add-metallibs.sh puts them into an existing sysroot.
METALLIB = '.metallib'
# ... but not the camera capture pipeline's (portrait, noise reduction, HDR, deghosting, photogrammetry, 3D
# reconstruction, video processing) nor the Xcode GPU-capture replayer's, about 40 MB packed: loaded by the capture
# daemons and the Camera pipeline, which have no camera on this port, not by an app's or SpringBoard's drawing
SKIP_METALLIB_OF = tuple(f'System/Library/PrivateFrameworks/{name}.framework/' for name in (
    'GPUToolsReplay', 'CMCapture', 'CMImaging', 'Portrait', 'HDRProcessing', 'Morpheus', 'Recon3D',
    'CorePhotogrammetry', 'Vista', 'GESS', 'MXI', 'DeepVideoProcessingCore', 'AppleCVAPhoto'))
SKIP_METALLIB_IN = ('/CCPortrait.bundle/', '/NRFV4.bundle/', '/NRFV2.bundle/', '/VideoDeghostingV3.bundle/')


def wanted_metallib(path):
    if KEEP_APPLE_ONLY:
        return True
    return not path.startswith(SKIP_METALLIB_OF) and not any(name in '/' + path for name in SKIP_METALLIB_IN)
# ... but '.dat' and '.bin' are also what these keep the tables in that the user interface is made with:
# the layouts of the on-screen keyboard, the emoji, the list of public domain suffixes
DATA_FRAMEWORKS = tuple(f'System/Library/{name}.framework/' for name in (
    'PrivateFrameworks/TextInputUI', 'PrivateFrameworks/CoreEmoji', 'Frameworks/CFNetwork'))
# never from applications: their sound, video and 3D content
SKIP_APP_EXT = ('.mov', '.mp4', '.m4a', '.caf', '.wav', '.heic', '.aiff', '.scnz', '.usdz')
# pictures the user interface is drawn with: small images, and asset catalogs up to this size
# (the one with all the SF Symbols alone is 170 MB and is left out)
MAX_ASSET_CATALOG = 4 << 20
# fonts: all but the very big ones; those two -- the colour emoji, 130 MB, and PingFang, the Chinese faces, 74 MB --
# are taken and made smaller after extraction (unpack.sh, ../tools/font_trim.py): the emoji without their biggest
# bitmaps (59 MB), PingFang with the regular and semibold faces for simplified, traditional and Hong Kong Chinese,
# both those programs name and the hidden ones (".PingFangSC-Regular") the system font falls back to
# (26 MB). Without them Safari and the rest drew emoji and Chinese characters as boxes with a question mark.
MAX_FONT = 25 << 20
TRIMMED_FONTS = ('System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc',
                 'System/Library/Fonts/LanguageSupport/PingFang.ttc')
# string tables hold every language; the biggest (Shortcuts' actions, Home) are 5-12 MB
MAX_STRING_TABLE = 16 << 20
# applications waiting here to be installed by the first boot go to /Applications instead
STAGED_APPS = 'private/var/staged_system_apps/'
# frameworks whose pictures are wanted whatever their size
ALL_PICTURES = ('System/Library/PrivateFrameworks/ProgressUI.framework/',)
# ... and asset catalogs: the SF Symbols (UIKit crashes on the first system image without them)
BIG_CATALOGS = ('System/Library/PrivateFrameworks/SFSymbols.framework/',)
# taken whole in principle, but not these: big and of no use until much later
# (/Library is the wallpapers and a few tables of regional settings, apart from the sounds)
HEALTH = 'System/Library/Health/'
SKIP_PREFIX = ('usr/share/tokenizer/', 'usr/share/firmware/', 'usr/share/mecabra/',
               'usr/standalone/', 'Library/Audio/', 'Library/Ringtones/',
               HEALTH + 'Assets/FitnessUIAssets.bundle/') + SKIP_APPLE_ONLY + \
    tuple(f'System/Library/{name}/' for name in SKIP_SYSTEM_LIBRARY)
BIG_PROGRAM = 10 << 20           # daemons bigger than this (locationd, lskdd, ...) are not on the way to the screen
# sound, outside the directories above: the virtual audio device, a plug-in that all of mediaserverd's
# audio goes through, with its tunings (signal processing chains) for this model and for any, and the
# system's own sounds. Not the neural networks among the tunings, nor the sounds of the watch.
AUDIO = ('Library/Audio/Plug-Ins/', 'Library/Audio/Tunings/D22/', 'Library/Audio/Tunings/Generic/',
         'System/Library/Audio/UISounds/')
SKIP_AUDIO = ('System/Library/Audio/UISounds/nano/',)
KEEP_LANG = ('en', 'Base', 'English', 'en_US', 'en_GB')
# the iOS-26 cryptex symlinks (System/Cryptexes/* -> ../../private/preboot/Cryptexes/*): structural,
# kept whatever they point at, because the rootfs and the dyld cache path resolve through them
CRYPTEX_LINKS = 'System/Cryptexes/'


def entries(listing):
    """Yield one dict per entry of a 7z technical listing."""
    entry = {}
    for line in open(listing, errors='replace'):
        line = line.rstrip('\n')
        if not line:
            if 'Path' in entry and 'Mode' in entry:
                yield entry
            entry = {}
        elif ' = ' in line or line.endswith(' ='):
            key, _, value = line.partition(' =')
            entry[key] = value[1:] if value.startswith(' ') else value
    if 'Path' in entry and 'Mode' in entry:
        yield entry


def mode_bits(text):
    """'-rwsr-xr-x' -> st_mode"""
    kinds = {'-': stat.S_IFREG, 'd': stat.S_IFDIR, 'l': stat.S_IFLNK}
    if text[0] not in kinds:
        return None
    mode = kinds[text[0]]
    for i, ch in enumerate(text[1:10]):
        if ch in 'rwxst':
            mode |= 0o400 >> i
        if ch in 'sS' and i == 2:
            mode |= stat.S_ISUID
        if ch in 'sS' and i == 5:
            mode |= stat.S_ISGID
        if ch in 'tT' and i == 8:
            mode |= stat.S_ISVTX
        if ch in 'ST':
            mode &= ~(0o400 >> i)
    return mode


def wanted_file(path, size):
    lproj = re.search(r'/([^/]+)\.lproj/', path)
    if lproj and lproj.group(1) not in KEEP_LANG:
        return False
    if path.startswith(AUDIO):
        return not path.startswith(SKIP_AUDIO) and '.espresso.' not in path
    if path.startswith(SKIP_PREFIX):
        return False
    if path.endswith(METALLIB):
        return wanted_metallib(path)
    if path.startswith('System/Library/Fonts/'):
        return size <= MAX_FONT or path in TRIMMED_FONTS
    if path.startswith(ALL_PICTURES) and path.lower().endswith('.png'):
        return True
    if path.startswith(('usr/libexec/', 'usr/sbin/')):
        return size <= BIG_PROGRAM
    if path.startswith(('Applications/', HEALTH)):
        return not path.lower().endswith(SKIP_APP_EXT) and '.mlmodelc/' not in path
    if path.startswith(FRAMEWORKS):
        if re.search(r'/(XPCServices|Support|Helpers)/', path):
            return size <= 32 << 20 and not path.lower().endswith(('.png', '.car', '.loctable', '.caml'))
        if path.endswith('.car'):
            return size <= MAX_ASSET_CATALOG or path.startswith(BIG_CATALOGS)
        if path.endswith('.loctable'):
            return size <= MAX_STRING_TABLE
        if path.startswith(DATA_FRAMEWORKS) and path.lower().endswith(('.dat', '.bin')):
            return size <= SMALL
        return size <= SMALL and not path.lower().endswith(SKIP_EXT) and '.mlmodelc/' not in path
    return path.startswith(WHOLE)


def main():
    listing, files_out, manifest_out = sys.argv[1:4]
    meta = {}                   # path in our image -> (mode, uid, gid, target, size)
    source = {}                 # ... -> path in Apple's image, where that is another
    for e in entries(listing):
        mode = mode_bits(e['Mode'])
        path = e['Path']
        if mode is None:
            continue
        if path.startswith(STAGED_APPS):
            source['Applications/' + path[len(STAGED_APPS):]] = path
            path = 'Applications/' + path[len(STAGED_APPS):]
        elif path.startswith('private/var/'):
            continue
        meta[path] = (mode, int(e.get('User ID') or 0), int(e.get('Group ID') or 0),
                      e.get('Symbolic Link', ''), int(e.get('Size') or 0))

    chosen = set()
    for path, (mode, uid, gid, target, size) in meta.items():
        if stat.S_ISREG(mode):
            keep = wanted_file(path, size)
        elif stat.S_ISLNK(mode):
            # a symlink costs nothing: keep every one that lives where we take anything at all.
            # System/Cryptexes/{OS,App,ExclaveOS,Supplemental} are the structural links iOS 26 ships
            # (-> ../../private/preboot/Cryptexes/...): the dyld shared cache and dozens of rootfs
            # framework symlinks resolve THROUGH them, so keep them even though System/ (bare) is not
            # in WHOLE -- without them /System/Cryptexes/OS/.../com.apple.dyld does not resolve.
            keep = '/' not in path or path.startswith(CRYPTEX_LINKS) or \
                path.startswith(WHOLE) and not path.startswith(SKIP_PREFIX)
        else:
            # (the sound plug-ins' too: Library/Audio is skipped but for AUDIO, and without its en.lproj the
            # virtual audio device's port names came out NULL -- mediaserverd died naming the microphone)
            lproj = re.search(r'/([^/]+)\.lproj$', path)
            keep = '/' not in path or path in ('private/var', 'private/tmp', 'private/etc', 'private/preboot',
                                               'private/xarts', 'private') or \
                bool(lproj) and lproj.group(1) in KEEP_LANG and \
                (path.startswith(WHOLE) and not path.startswith(SKIP_PREFIX) or
                 path.startswith(AUDIO) and not path.startswith(SKIP_AUDIO))
        if keep:
            chosen.add(path)
    # every directory on the way to something chosen
    for path in list(chosen):
        while '/' in path:
            path = path.rsplit('/', 1)[0]
            chosen.add(path)

    total = count = 0
    with open(files_out, 'w') as files, open(manifest_out, 'w') as manifest:
        for path in sorted(chosen):
            if path not in meta:
                continue
            mode, uid, gid, target, size = meta[path]
            manifest.write(f'{mode:o}\t{uid}\t{gid}\t{path}\t{target if stat.S_ISLNK(mode) else ""}\n')
            if stat.S_ISREG(mode):
                files.write(source.get(path, path) + '\n')
                total += size
                count += 1
    print(f'select-system: {count} files, {total / 1048576:.0f} MB; {len(chosen)} entries in the manifest')


if __name__ == '__main__':
    main()
