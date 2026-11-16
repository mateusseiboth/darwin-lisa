# HomeButtonType: why iOS lays out lisa as a phone with a home button (2026-11-13, host only)

Nothing was run on the phone and nothing was written to it. The root `efed09de` is unchanged: the fix is in the loader
(vendor_boot), not in the root.

## Cause

MobileGestalt answers `HomeButtonType` = 0 ("home button"). A real iPhone 13 mini answers 2 ("no home button").

- **Where the answer comes from.** In libMobileGestalt of 23F84, key `JwLB44/jEB8aFDpXQ16Tuw` = HomeButtonType.
  - Its table entry is at `0x1f15ad8c8`. The producer is `sub_1b1c26df0`: it reads `IODeviceTree:/buttons`
    `home-button-type`, and it answers 0 when the node or the property is missing.
  - The entry's cache index is `0xffff`, so the answer is never cached. It is read from the device tree every time.
  - Consequence: `com.apple.MobileGestalt.plist` cannot change it. `CacheExtra` and `CacheData` are only consulted for
    keys that have a cache index (`sub_1b1c1aa68`). Editing the plist on darwin_var does nothing here.
- **What our device tree has.** The real D16 tree (`ipsw/d16-dtree/DeviceTree.d16ap.bin`) has `/buttons`
  `home-button-type = 2`, and so does D22. Our loader only copies the root and `/product` of the identity. It writes no
  `/buttons` node, so the answer is 0.
- **What reads it.**
  - **UIKit.** `+[UIDevice _hasHomeButton]` is `HomeButtonType != 2`. It reads the MG key directly or through
    `UISDeviceContext` (`_UISDeviceContextHomeButtonTypeKey`).
  - **SpringBoard.** `SBHScreenTypeForCurrentDevice` (SpringBoardHome) takes the screen bounds, scale,
    `SBFEffectiveArtworkSubtype` and `SBFEffectiveHomeButtonType` (`BSPlatform homeButtonType`, which is the same MG
    key). It looks for an exact match (`SBHGetScreenSpecification`, whose last field is 1 for a home-button type and 2
    for the rest), then falls back by size.
    - Today (375x812@3, 2340, HBT 0) nothing matches exactly. The home-button fallback gives type 1, **"iPhone 6
      (N61)"**.
    - With HBT 2 the spec matches exactly: type `0xf`, **"iPhone 13 mini (D16)"** (`SBDisplayMetrics[0x50]` =
      375x812, scale 3, artwork `0x924` = 2340).
  - **The notch.** `+[UISDisplaySingleRectShape sb_thisDeviceDisplayShape]` gives `sb_displayShapeForScreenType:`.
    - Types 3 to 0x1e return a rect. Types 0, 1 and 2 return nil and log about the home affordance.
    - SpringBoard puts the shape in `UISDisplayContext.exclusionArea`, which apps receive.
  - **The status bar.** `+[_UIStatusBarVisualProvider_Phone visualProviderSubclassForScreen:]` reads
    `UIScreen._exclusionArea`:
    - nil gives `_UIStatusBarVisualProvider_LegacyPhone`, the classic layout seen on lisa;
    - a rect with y <= 0 gives `_UIStatusBarVisualProvider_Split`;
    - a rect with y > 0 gives `DynamicSplit`.
- **Matches what was seen.**
  - The status bar is classic: carrier, centred time (evening-session.md).
  - "SpringBoard runs the classic (home-button) layout: a swipe from the top-right opens Notification Center, and
    Control Center is the bottom edge" (gpu-device-test.md).

The values on a real 13 mini: HomeButtonType 2 (D16 `/buttons`), ArtworkDeviceSubType 2340 (D16 `/product`
`artwork-device-subtype` = `0x924`). The darwin_var cache already holds the second one (`oPeik/9e8lQWMszEjbPzng` ->
`ArtworkDeviceSubType: 2340`). DeviceClassNumber is not in the decision.

## The fix: `/buttons home-button-type` in the loader's device tree

| File | What | sha256 (16) |
|---|---|---|
| `loader/identity-soc.py` | also writes `#define IDENTITY_HOME_BUTTON_TYPE 2`, from the Apple tree's `/buttons` (D16 and D22: 2) | `2605c5c405ced1a9` |
| `loader/test_identity_soc.py` | tests on a synthetic tree (`python3 -I darwin-lisa/loader/test_identity_soc.py`: 6 OK) | `0e1d580c42e6dd88` |
| `loader/identity-buttons.patch` | the loader writes `/buttons` with only `home-button-type` (no compatible, so no driver matches it) when the header has the define | `4cdec471f8ed12b9` |
| `loader/build-standalone.sh` | applies the patch last; checks that the loader carries the property | |
| `loader/build-combined.sh` | refuses an identity header without the define | |
| `.work/identity/identity-d16.h` (Apple data, not in git) | re-run of identity-soc.py; the old one is `identity-d16.h.before-buttons` | `5c3332fd6d84d914` |

A loader built with the patch and the old header is byte for byte `loader-final17-cpus4.efi`, apart from the PE
timestamp. The images below differ from their base only in the loader and the epoch.

| Image | Base | Kernel, cpus | sha256 |
|---|---|---|---|
| `out/vendor_boot-lisa-final19-hbt.img` | final17-cpus4 (`2e7f0b85`, on vendor_boot_a) | rc3 `1a072069`, cpus=4 | `bc7a7ea25f6a22a360aa85accb266dbe6091e4b3aa44bf91c0c83959d796eeda` |
| `out/loader-usb-lisa-final19-hbt.hkub` | the same HKUB, for RAM | | `4dc95f262e4b402d45d847f43750c7745049cdf9cb6ac99019407226c1490f7d` |
| `out/vendor_boot-lisa-final20-sel7-hbt.img` | final18-sel7 (`c5f535eb`) | s8-select `0270ca71`, cpus=7 | `9b5508914e52489092a97d38027eae89869df7b6b9a86bf0819292d47ef69dfb` |
| `out/loader-usb-lisa-final20-sel7-hbt.hkub` | the same HKUB, for RAM | | `59cc64d0dc5e6a5836984533058d03b615d0ace140ddd32abd4aee4d864e0d33` |

The build line is final17's (or final18's) with the new NAME, for example:
`GEOMETRY= CPUS=4 NAME=final19-hbt KERNEL=research/hackios-xnu-kernel/kernel-16k-consolidated-rc3.bak
EXTRA_ARGS="noidle=1 tz=America/Campo_Grande gpu=1 metal=1 dlisa_async=1 metal_spec_wait=1
interrupt-masked-debug-mode=0 metal_all=1" darwin-lisa/loader/build-combined.sh`. The line is 450 characters, and
word for word it differs from its base only in `epoch=`.

Use final19 if vendor_boot_a stays on final17 tonight. Use final20 if S8-A put final18-sel7 there.

No `ls_reset` is needed: HomeButtonType is not cached. The root, darwin_var and the MobileGestalt cache stay as they
are.

## The test (one boot, with root efed09de already flashed)

Pick one of the two ways.

**A. RAM only, nothing written.** Phone in pmOS or in the loader:

```sh
cd ~/Dev/mobile && bash darwin-lisa/tools/bootos.sh uboot
fastboot stage out/loader-usb-lisa-final19-hbt.hkub
fastboot oem run:"hakios load fastboot"
fastboot continue        # leaves fastboot_screen; U-Boot's "waiting for the host" fastboot comes up
fastboot continue        # hakios boot: the new loader with final19's payload
```

Then send nothing. After 10 s `autoboot` starts iOS with final19's line, the same as the standalone boot of final17
plus `/buttons`. Look at the screen at 4 to 5 min.

**B. Flash.** `PMOS_PASS=... bash darwin-lisa/tools/flash-vendorboot.sh out/vendor_boot-lisa-final19-hbt.img` (it reads
back `bc7a7ea2`). Then do a cold boot from pmOS with no host command in the autoboot window. Host runs
(`gpu-quietrun.sh`) use this loader too.

What to look at:

1. **Status bar, split.** The time is at the top left and battery/Wi-Fi at the top right, with nothing in the middle
   over the punch-hole. Before: carrier, centred time.
2. **Home indicator.** A bar at the bottom edge on the lock screen and in apps. The lock screen says to swipe up to
   open.
3. **Gestures** (by finger, or `touch_seq`):
   - `up` (swipe up from the bottom) goes home;
   - `sw` (swipe up and hold) opens the card switcher;
   - `cc` (top right, down) opens Control Center;
   - `nc` (top left, down) opens Notification Center.
   Before, the top right gave Notification Center and Control Center came from the bottom edge.
4. **Apps.** Content starts below the notch (top safe area) and above the indicator. The home screen has the 13 mini's
   grid.
5. In the log: no SpringBoard crash loop.

Note for the GPU sessions: `touch_seq` `cc@...` now opens Control Center (it opened Notification Center before), so the
runs measure a different screen than Sessions 7 to 9.

## Rollback

- A: reboot (`fastboot reboot`, or Power). Nothing was written.
- B: `PMOS_PASS=... bash darwin-lisa/tools/flash-vendorboot.sh out/vendor_boot-lisa-final17-cpus4.img` (`2e7f0b85`). The
  script also keeps a backup of what it overwrote in `backups/`. If final18-sel7 was there, use
  `out/vendor_boot-lisa-final18-sel7.img` (`c5f535eb`).
- Nothing on darwin_var depends on the change. The home screen layout of the 13 mini and of the "iPhone 6" are both
  4x6, and SpringBoard re-lays it if needed.

## What was not done

- No plist edit on darwin_var: it cannot work for this key (see Cause).
- A libdlisa hook of `MGGetSInt32Answer` would also work, but it needs a new root.
- The other `/buttons` answers of MobileGestalt (button names, ringer, side button) stay at their missing-node values,
  as before.

## Part 2 (2026-11-16, host only): the status bar shows "rdar:45025538"

poster-f3 (vendor_boot final19-hbt, root 2ef33580, `research/hackios-xnu-kernel/logs/poster-f3-q1..q5.png`):
- passed: home indicator, the 13 mini's grid, no crash loop;
- failed: the status bar shows only an emoji and "rdar:45025538" at the top left, with no time and no battery;
- at +320 s: no status bar, the home screen shifted up, and one row of icons cut.

### Cause: the 13 mini's notch on the iPhone X's framebuffer

- **What the text is.** The text is UIKit's fallback for "No status bar visual provider for screen %@ (width, scale,
  idiom, cornerRadius). Please see rdar://45025538."
- **How UIKit picks a split status bar.** With a notch, `+[_UIStatusBarVisualProvider_Split
  visualProviderSubclassForScreen:]` tries 9 subclasses. A subclass is taken only when both of these hold (block
  `sub_18a8065bc`):
  - its `nativeDisplayWidth` equals the screen's `_nativeDisplayBounds` width;
  - its `notchSize` equals the exclusion rect times `_nativeScale / referenceScreenScale` (within the tolerance).
- **The subclasses that matter** (UIKitCore constants):

  | Class | native width | reference scale | notch (points) |
  |---|---|---|---|
  | `Split1125` | 1125 | the screen's | 209 x 30 |
  | `Split1080` | 1080 | 2.88 | 226 x 34.33 |
  | `Split1080_Wide` | 1080 | 2.88 | 174 x 37.33 |

- **Our screen.** It is the kernel's default framebuffer: 1125x2436, 375x812 points at 3x, nativeScale 3. Only
  `Split1125` passes the width test.
- **The notch SpringBoard computes.** `sb_displayShapeForScreenType:` takes a rect in native pixels and divides it by
  `__SBHNativeScaleForScreenType`. That scale is 2.88 x 1.0417 = 3 for the minis.
  - Type 0xf, "iPhone 13 mini (D16)", the type HBT 2 gave: x 301.5, w 522, h 112, so 174 x 37.33. That is the real
    13 mini's notch, which fits `Split1080_Wide` only on a 1080-pixel panel at nativeScale 2.88.
  - Type 3, "iPhone X (D22)": x 249, w 627, h 90, divided by 3 gives 209 x 30, which is `Split1125`.
- **Result.** A 13-mini notch on a 1125-pixel screen matches no subclass, and UIKit falls back to the placeholder.
- **The +320 s screen.** Most likely the same broken status bar, relaid. Re-check it after the fix.
- **Why artwork alone decides the type.** SpringBoard needs an exact spec. Type 3's spec is 375x812, scale 3, artwork
  **2436**, no home button, and the only thing that differs from today is the artwork (2340, the 13 mini's).

### Fix: artwork-device-subtype 2436 while the framebuffer is the iPhone X's

| File | What | sha256 (16) |
|---|---|---|
| `loader/identity-artwork.patch` | with `IDENTITY_ARTWORK_SUBTYPE`, `dt_identity` writes `/product artwork-device-subtype` with it instead of the identity's (as `artwork-display-gamut` already is). Without the define the loader is byte for byte final19's (apart from the PE timestamp) | `db6b52b5bd1ce44d` |
| `loader/build-standalone.sh` | `ARTWORK=` adds the define to the build's copy of the identity header (the Apple header itself is untouched); applies the patch | |
| `loader/build-combined.sh` | `ARTWORK=2436` by default when `GEOMETRY` is empty (the kernel's 1125x2436); `ARTWORK=` keeps 2340 | |

The rest of the identity stays the 13 mini's: model, D16AP, SoC, HomeButtonType 2. With these values SpringBoard takes
type 3, the notch becomes 209 x 30, and UIKit picks `_UIStatusBarVisualProvider_Split1125`, the iPhone X's split bar. A
real 13 mini uses `Split1080_Wide`. Getting that one would need the kernel to report a 1080-pixel panel with
nativeScale 2.88, a display driver change.

| Image | Base | sha256 |
|---|---|---|
| `out/vendor_boot-lisa-final22-x2436.img` | final19-hbt + artwork 2436 (rc3, cpus=4; line = final19's but the epoch) | `1324baa20649697233e2fc923fe6d78c47ea0da5f877478e46d7bbd92ea46eaa` |
| `out/loader-usb-lisa-final22-x2436.hkub` | the same HKUB | `ace975914af901672caf8a1388dec591408a24bbc836e8243b0e3a6a0daaf1ea` |

The build line was `GEOMETRY= CPUS=4 NAME=final22-x2436
KERNEL=research/hackios-xnu-kernel/kernel-16k-consolidated-rc3.bak` plus final17's `EXTRA_ARGS`. The name final21 was
already taken by another worker's `final21-s14-idle`. That image was built from the same tree, but without
`identity-artwork.patch` and without ARTWORK, so it carries the bug.

**The MobileGestalt cache must go once.** ArtworkTraits (`oPeik/9e8lQWMszEjbPzng`, cache index 0x24) is cached.
`SBFEffectiveArtworkSubtype` reads `ArtworkTraits.ArtworkDeviceSubType`, so a kept cache would still say 2340.
`ls_reset=1` removes the cache (init26); the identity stamp does not, because hw.machine and hw.model are unchanged.

### Test (vendor_boot write, then one host run)

```sh
cd ~/Dev/mobile && export PMOS_PASS=... SSHPASS=$PMOS_PASS
PMOS_PASS=$PMOS_PASS darwin-lisa/tools/flash-vendorboot.sh out/vendor_boot-lisa-final22-x2436.img   # reads back 1324baa2
KERNEL=kernel-16k-consolidated-rc3.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh hbt-x1 "dlisa_unlock=1 dlisa_noidle=1 display=1 console_open=1 lisa=1 cpus=4 keystore=1 init_run=/usr/libexec/usermanagerd:--init gpu=1 metal=1 dlisa_async=1 metal_spec_wait=1 interrupt-masked-debug-mode=0 metal_all=1 ls_reset=1 touch_seq=nc@240,up@270,cc@300,up@330" "200 255 285 315 345"; bash darwin-lisa/tools/bootos.sh pmos
```

The line is 428 characters. `ls_reset=1` makes the first boot slower, because LaunchServices seeds again.

Pass:
- every screenshot has the split bar: time at the top left, battery and Wi-Fi at the top right, no "rdar:45025538";
- `nc` at +240 opens Notification Center, `cc` at +300 opens Control Center, `up` goes home;
- the +345 screenshot keeps its status bar, with no shifted or cut row;
- in the log, `init26: VAR ls_reset: ... MobileGestalt cache removed`.

Later boots do not need `ls_reset`. Then do a cold boot with no host command, with a screenshot at +4/+5 min.

The touch_seq line was not checked against a run. If `touch_seq` was renamed since (Session 11's libdlisa), use that
session's gesture argument instead.

Rollback:
- `tools/flash-vendorboot.sh out/vendor_boot-lisa-final19-hbt.img` (`bc7a7ea2`) or final17 (`2e7f0b85`).
- The cache then still says 2436 until one more host run with `ls_reset=1`.
- With final19, that cache gives type 3 and Split1125, which works. With final17, HBT 0 makes the artwork irrelevant to
  the status bar.

## Part 3 (2026-11-16, host only): still "rdar:45025538" with artwork 2436

hbt-x1 (vendor_boot final22-x2436 `1324baa2`, root 2ef33580, `ls_reset=1`, which removed the MobileGestalt cache).
Every capture still shows the placeholder, Control Center included, and the lock screen shows a red bar
(`research/hackios-xnu-kernel/logs/hbt-x1-q1..q5.png`). The log has no line that says what the system decided. Part 2's
expectation was wrong.

### What was checked (static, 23F84)

- **(a) The device tree carries 2436.** The final22 loader's `dt_identity` has the constant (`mov w28, #0x984` at
  `0x9bb0` of `loader.obj`). The patch replaces the identity's `artwork-device-subtype` there and nowhere else.
- **(b) Where the artwork comes from.**
  - MobileGestalt's ArtworkTraits (`sub_1b1c250b4`) reads `IODeviceTree:/product artwork-device-subtype` and nothing
    else. It is cached (index 0x24), and `ls_reset=1` removed that cache.
  - SpringBoard reads it through `SBFEffectiveArtworkSubtype` (`MGCopyAnswer("oPeik/9e8lQWMszEjbPzng")
    ["ArtworkDeviceSubType"]`). The device emulation override there is not configured.
  - The hbt-x1 home screen lost its icon images, which fits a changed artwork, but nothing in the log proves it.
- **(c) What UIKit compares.**
  - `UIScreen._nativeDisplayBounds` is `FBSDisplayConfiguration.pixelSize`.
  - `UIScreen._nativeScale` is that width divided by the reference width.
  - The framebuffer reports 1125x2436 everywhere: timing, `GetDisplaySize`, default surface.
  - On those numbers, SpringBoard's type 3 notch (`sb_displayShapeForScreenType:`, 627 x 90 px divided by 3) is 209 x 30
    and should pass `Split1125`. It did not, so at least one runtime value differs from the static reading (pixel size,
    scale, notch or screen type), and only the phone can tell which.
- **UIKit has an override.**
  - The resolver (`sub_18a7e95a0`, the function that logs "No status bar visual provider ... rdar://45025538") first
    reads, for the embedded screen, `UIStatusBarVisualProviderClassName` from `NSUserDefaults` suite
    `com.apple.UIKit`.
  - It takes any class that conforms to the provider protocol and logs "WARNING: Using overridden status bar visual
    provider class: %@".
  - No internal-build check guards it.

### Fix: SpringBoard tells UIKit which split bar to use, and says what UIKit chooses from

libdlisa, SpringBoard only (`userland/inject/status-bar.c`, `dlisa-inject.c` `startStatusBarFix`):

- **The fix.** At start SpringBoard writes `com.apple.UIKit` `UIStatusBarVisualProviderClassName`, through cfprefsd,
  for the user mobile, so every app of the user reads it. The value is the split bar made for the display's width
  (`fbwidth=`, default 1125):

  | Display width | Class |
  |---|---|
  | 1125 | `_UIStatusBarVisualProvider_Split1125` |
  | 1080 | `Split1080_Wide` (13 mini) |
  | 1170 | `Split1170` |
  | 1242 | `Split1242` |
  | 1284 | `Split1284` |
  | 828 | `Split828` |

  `dlisa_sbprov=0` removes the key again. The key stays on darwin_var; a root without this libdlisa still uses it.
- **The probe.** 30 s in, two lines:
  - `dlisa[SpringBoard]: screen: artwork N, home button type N, SpringBoard type N (name)`;
  - `dlisa[SpringBoard]: screen: WxH points at S, native WxH at S, corner R, notch CLASS x,y WxH; UIKit's own split
    bar: CLASS or none (rdar:45025538)`.
  Together they settle (b) and (c) on the next boot: if UIKit's own pick is "none", the numbers on that line name the
  mismatch.
- **Tests.** Host test `userland/inject/test/status-bar-test.c` (10 checks); `sh userland/inject/test/run-tests.sh`:
  all 0 failures.

| Piece | What | sha256 |
|---|---|---|
| `userland-26/out/libdlisa-sbprov/libdlisa.dylib` | libdlisa of 2ef33580 (ef2a85a2, source 62dea9b, rebuilt byte for byte first) + this change only | `f3438d682f642f08389c5c61a1686fed67ea935a334ffb08c74d626dfb1cf3b4` |
| root `userland-26/out/system-26.apfs.full-2fa0b667` | 2ef33580 with only libdlisa changed: hard-link copy of `out/sysroot-26` (shared sysroot untouched: libdlisa f61dbf9b, glass7) with glass9 `HakiosMetal` 13ea2c9a and this libdlisa, `mkapfs.py -z` with `extract/system.manifest`, padded to 32 MiB. 61212 files, 24194 directories, 821 symlinks (as 2ef33580), 4,999,610,368 B. Read back: libdlisa f3438d68, `HakiosMetal` 13ea2c9a, `Info.plist` 3769fa7f, `dlisa-inject.conf` 86a53398, launchd d2506319 | `2fa0b667c5870824e7b027e992addb0e241afaf8361b413ba759160c7f2e8e02` |

HEAD's libdlisa (with df57e4f and the launch probe) gets the same change in the same commit. Only the root above is
the 2ef33580 build.

### Test (root write, then one host run; vendor_boot stays final22-x2436)

```sh
cd ~/Dev/mobile && export PMOS_PASS=... SSHPASS=$PMOS_PASS
PMOS_PASS=$PMOS_PASS darwin-lisa/tools/flash-root.sh userland-26/out/system-26.apfs.full-2fa0b667     # reads back 2fa0b667
KERNEL=kernel-16k-consolidated-rc3.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh hbt-x2 "dlisa_unlock=1 dlisa_noidle=1 display=1 console_open=1 lisa=1 cpus=4 keystore=1 init_run=/usr/libexec/usermanagerd:--init gpu=1 metal=1 dlisa_async=1 metal_spec_wait=1 interrupt-masked-debug-mode=0 metal_all=1 touch_seq=nc@200,up@230,cc@260,up@290" "150 215 245 275 305"; bash darwin-lisa/tools/bootos.sh pmos
grep -a -E "dlisa\[SpringBoard\]: (status bar|screen)" research/hackios-xnu-kernel/logs/hbt-x2-end.log
```

This run needs no `ls_reset`: the artwork cache was rebuilt by hbt-x1.

Pass:
1. The log has `status bar: com.apple.UIKit UIStatusBarVisualProviderClassName = _UIStatusBarVisualProvider_Split1125
   (saved)`. On a later boot it says `already so`.
2. The two `screen:` lines are there. They show artwork 2436, home button type 2, a SpringBoard type, the native size
   and scale, and the notch. Their "UIKit's own split bar" value is the root cause for the record, even with the fix
   in place.
3. Every capture: time at the top left, battery and Wi-Fi at the top right, no "rdar", no red bar on the lock screen.
   The same inside Control Center (`cc@260`).
4. Settings (if opened) has the same bar, since apps read the same default.

If pass 1 says `NOT saved`, the write failed and nothing else changed.

Rollback:
- `tools/flash-root.sh userland-26/out/system-26.apfs.full-2ef33580`.
- The key stays on darwin_var and keeps the Split1125 bar, which is harmless.
- To remove it, boot this root once with `dlisa_sbprov=0`.

