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

## Part 4 (2026-11-16, host only): the probe's numbers, and the fix that fits them

ubwc-A (root 8739d3b5, libdlisa 174255e0 = part 3) printed:

```
status bar: com.apple.UIKit UIStatusBarVisualProviderClassName = _UIStatusBarVisualProvider_Split1125 (saved)
screen: artwork 2436, home button type 2, SpringBoard type 4 (iPhone X (D22, Zoomed))
screen: 360x800 points at 3.00, native 1080x2400 at 3.000, corner 33.33, notch UISDisplaySingleRectShape 70.83,0.00 178.35x25.60; UIKit's own split bar: none (rdar:45025538)
```

The screenshots still showed "rdar:45025538".

### Cause: the screen is 360 x 800 points, not 375 x 812

- **Where the size comes from.** `lisa=1` carries the kernel's built-in `fbwidth=1080 fbheight=2400`
  (`kernel/drivers/bootargs/lisa-defaults-wire.py`). Every line since final14 has `lisa=1`, so the display is the
  panel's own 1080x2400 at 3x: 360 x 800 points, a size no iPhone has.
- **SpringBoard.** No spec matches exactly. With no home button its size fallback ends at type 4, "iPhone X (D22,
  Zoomed)", whatever the artwork.
  - Type 4's notch is the X's 249, 0, 627 x 90 pixels divided by the zoomed X's native scale 1125/320 = 3.5156. That
    gives 70.83, 0, 178.35 x 25.60, exactly the probe's numbers.
- **UIKit.** At 1080 native pixels only `Split1080` and `Split1080_Wide` pass the width test (reference scale 2.88). The
  ratio is 3/2.88 = 1.0417, so the notch they would see is 185.8 x 26.7. `Split1080` wants 226 x 34.33 and
  `Split1080_Wide` wants 174 x 37.33, so neither matches and UIKit shows the placeholder.
- **Why part 2 and part 3 missed.**
  - Part 2 assumed the kernel's 1125x2436.
  - Part 3 wrote Split1125 asynchronously at SpringBoard's start, after UIKit had already resolved its bars (UIKit
    reads the key when it makes a bar). The key was saved, so the next start would have used it.
  - Split1125 is the wrong class for a 1080-pixel screen in any case.
- **Side note.** `Split1080_Wide`'s notch (174 x 37.33 at 2.88) is exactly SpringBoard's 13 mini notch (type 0xf,
  522 x 112 pixels divided by 3) times 3/2.88. With type 0xf on this 1080-pixel screen at 3x, UIKit would choose the
  13 mini's bar by itself. Type 0xf only matches at 375 x 812 points, though.

### Options weighed

| Option | Effect | Cost |
|---|---|---|
| A. libdlisa writes `Split1080_Wide` (the 13 mini's bar) for 1080 pixels, synchronously at SpringBoard's start | the 13 mini's own split bar on the panel's 1:1 picture; every app of the user | nothing on the display; SpringBoard keeps "iPhone X Zoomed" (320-point icon layout) |
| B. `fbwidth=1125 fbheight=2436` + artwork 2436 (final22) | the system is natively an iPhone X: type 3, notch 209 x 30, Split1125, no override | 5.7 % more pixels to draw, and the 0.96 x 0.985 scale onto the panel (bilinear: 3.6 ms a frame, 11 ms one in ten, per the framebuffer driver's notes), not 1:1; the GPU sessions' numbers change |
| C. a 1080 x 2340 panel at 2.88 (375 x 812 points scaled, as on the 13 mini) | the real 13 mini: type 0xf, Split1080_Wide by UIKit's own choice | a display driver change (a scaled mode, native size separate from the mode) plus 60 letterboxed rows; not done |

**Chosen: A.** It keeps the panel 1:1. B stays available (`LISA_DEFAULTS=0` or an explicit
`fbwidth=1125 fbheight=2436` with `ARTWORK=2436`). C is the real fix of the screen type, later.

### Changes

- `userland/inject/status-bar.c`: the width is `fbwidth=` from the line, else 1080 with `lisa=1`, else 1125. 1080 gives
  `_UIStatusBarVisualProvider_Split1080_Wide`. New `findStatusBarProviderForWidth`. Test: 15 checks, all
  `run-tests.sh` files at 0 failures.
- `dlisa-inject.c`: the key is set in SpringBoard synchronously at start, so the first start already uses it, and
  saved to cfprefsd on the side. 30 s in, the width UIScreen reports is checked against the boot-arg guess; when it
  differs, the right class is saved for the next start ("from the screen's width").
- `loader/build-combined.sh`: no more `ARTWORK=2436` default. That default was wrong with `lisa=1` (part 2's premise).
  The artwork stays the identity's 2340.

**vendor_boot.** The artwork does not matter at 360 x 800, so go back to the identity's 2340 with images already built:
- rc3 / cpus=4: `out/vendor_boot-lisa-final19-hbt.img` (`bc7a7ea2`). It equals final22-x2436 apart from the artwork.
- s14 / cpus=7: `out/vendor_boot-lisa-final21-s14-idle.img` (`6ce52f26`), which has HBT and no artwork.

final22/final23 (2436) also work with this fix, but they show the X's artwork. No new vendor_boot is needed.

| Piece | What | sha256 |
|---|---|---|
| `userland-26/out/libdlisa-sbprov2/libdlisa.dylib` | 8739d3b5's libdlisa (174255e0, source 8e30b5c, rebuilt byte for byte at the canonical path) + this change only | `bf0619c67fd137819711019550ea11720143cf5dbb72686a0eb247606deb2687` |
| HEAD's libdlisa (this commit, with bluetooth and the text probe) | `build-inject.sh` | 71948b30 (built here, not installed) |
| root `userland-26/out/system-26.apfs.full-f1132de1` | 8739d3b5 with only libdlisa changed. Built from a hard-link copy of `out/sysroot-26` (copy deleted after; the shared sysroot is untouched) holding 8739d3b5's five own files (HakiosMetal glass11 9e29ff9c, launchd dbeb1522, InterfaceNamer `Info.plist` aa6f1292, `dlisa-inject.conf` dd9abccb, libdlisa replaced by bf0619c6), then `mkapfs.py -z` with `extract/system.manifest`, padded to 32 MiB. 61212 files, 24194 directories, 821 symlinks, 4,999,610,368 B. The five files were read back from the image | `f1132de17144ce723356524077b4a417e1ed448dfdb2ae487fe1c29bed4e72cb` |

### Test (root write; vendor_boot final19-hbt, or keep final22, which also works)

```sh
cd ~/Dev/mobile && export PMOS_PASS=... SSHPASS=$PMOS_PASS
PMOS_PASS=$PMOS_PASS darwin-lisa/tools/flash-root.sh userland-26/out/system-26.apfs.full-f1132de1        # reads back f1132de1
# optional, the identity's artwork back: PMOS_PASS=$PMOS_PASS darwin-lisa/tools/flash-vendorboot.sh out/vendor_boot-lisa-final19-hbt.img  # bc7a7ea2
KERNEL=kernel-16k-consolidated-rc3.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh hbt-x3 "dlisa_unlock=1 dlisa_noidle=1 display=1 console_open=1 lisa=1 cpus=4 keystore=1 init_run=/usr/libexec/usermanagerd:--init gpu=1 metal=1 dlisa_async=1 metal_spec_wait=1 interrupt-masked-debug-mode=0 metal_all=1 touch_seq=nc@200,up@230,cc@260,up@290" "150 215 245 275 305"; bash darwin-lisa/tools/bootos.sh pmos
grep -a -E "dlisa\[SpringBoard\]: (status bar|screen)|Using overridden status bar" research/hackios-xnu-kernel/logs/hbt-x3-end.log
```

Pass:
1. `status bar: ... = _UIStatusBarVisualProvider_Split1080_Wide (saved, from the boot-args)`. It replaces part 3's
   Split1125 on darwin_var. On the next start it reads `already so`.
2. No second `status bar:` line "from the screen's width". If one appears, the boot-arg guess was wrong; it names the
   class saved for the next start.
3. The `screen:` lines are unchanged in substance: 360x800, native 1080x2400, type 4, UIKit's own choice none. They
   record the cause, and UIKit's own choice is not what is drawn.
4. Every screenshot from the first one shows the time at the top left and battery and Wi-Fi at the top right. No
   "rdar", no red bar on the lock screen, and the same inside Control Center (`cc@260`).
5. `Using overridden status bar visual provider class` appears if UIKit's NSLog reaches the console. It is not
   required.

If a later run turns up an app without the bar: apps read the same key through cfprefsd, so `defaults` on the phone
(`com.apple.UIKit`) is where to look.

Rollback:
- `tools/flash-root.sh userland-26/out/system-26.apfs.full-8739d3b5`.
- The key stays on darwin_var (Split1080_Wide) and keeps working with the older root, whose libdlisa would write
  Split1125 again at its start.
- `dlisa_sbprov=0` on a boot of this root removes the key.


### Root int3 (2026-11-16): the combined test root with HEAD's libdlisa

The root is `userland-26/out/system-26.apfs.full-int3`, sha256
`122a0d5c02d8f1dceca3eae0a92d3da9764d0261aa1e99dbabc4187ceb28d7f4`, 4,999,610,368 B. Not written.

- **Base.** It is int2 (`517468c1`) with only libdlisa changed: fab777af becomes HEAD's **7249bc09** (HEAD 1963144,
  `userland/inject` equal to f408bc4).
  - That libdlisa carries the status bar fix of part 4, the text probe, the inert virtual BT and torch.
  - It was rebuilt from `git archive HEAD` at the canonical path and is deterministic: the working tree builds the same.
  - `sh userland/inject/test/run-tests.sh`: 11 files, 0 failures.
  - The 71948b30 quoted earlier was a working tree in the middle of another worker's commit. HEAD now builds 7249bc09.
- **How it was built.**
  - int2's own files were found by a size diff of every regular file of the image against `out/sysroot-26`, plus
    launchd (same size).
  - Those files were then taken from int2: ld-groups 2beca308, conf e0704957, HakiosMetal glass11 9e29ff9c,
    InterfaceNamer `Info.plist` aa6f1292, libdlisa-motion d948f0d5, launchd dbeb1522.
  - Hard-link copy of the sysroot (deleted after; the shared sysroot is untouched), `mkapfs.py -z` with
    `extract/system.manifest`, padded to 32 MiB.
- **Read back.** The seven files above. The same size diff against the sysroot lists the same six files as int2. 61212
  files, 24194 directories, 821 symlinks.
- **The conf stays int2's e0704957.** HEAD's `dlisa-inject.conf` (440 B) has more entries; they are not in this root.
- `system-26.apfs.full-f1132de1` was deleted. int2 is kept.

## Part 5 (2026-11-16, host only): the key is not honoured; a direct fix of the notch

ctl2 ran on root 9a91ed47 (HEAD libdlisa) with vendor_boot final19-hbt. The log said
`UIStatusBarVisualProviderClassName = _UIStatusBarVisualProvider_Split1080_Wide (already so)`, yet every screenshot
(home, Control Center) still showed "rdar:45025538". The `screen:` lines were unchanged: artwork 2340, type 4, 360x800
at 3, native 1080, notch 70.83,0 178.35x25.60, UIKit's own pick none.

### Why the key does nothing

The resolver only consults it after `-[UIScreen _isEmbeddedScreen]`. It reads the key through an `NSUserDefaults` that
UIKit builds itself (`sub_1891d061c`, `initWithSuiteName:@"com.apple.UIKit"` plus a suite of its own), and whichever of
the two fails here, the `Phone -> Split` path never sees the class. Parts 3 and 4 were the wrong lever. Rather than
chase that further, the fix now acts on what UIKit compares.

### What UIKit compares, and what makes it match by itself

| Class | native width | reference scale | notch (points) |
|---|---|---|---|
| `Split1080` | 1080 | 2.88 | 226 x 34.33 |
| `Split1080_Wide` | 1080 | 2.88 | 174 x 37.33 |

- **The rule.** UIKit takes a split bar when its `notchSize` equals the screen's exclusion rect times
  `_nativeScale / referenceScreenScale`.
- **On this screen.** `_nativeScale` is 3, so the rect `Split1080_Wide` wants is 174 x 37.33 times 2.88/3, which is
  **167.04 x 35.84**, centred on 360 points at x **96.48**: the 13 mini's notch in this screen's points.
- **Why the 13 mini type alone would not do.** SpringBoard's own 13 mini notch (type 0xf: 522 x 112 pixels divided by
  native scale 0.96 x 3 x 1.0417 = 3) is 174 x 37.33, made for nativeScale 2.88. On this screen it would be off by 4 %.
- **Where the rect comes from.** SpringBoard puts it, once, in every display context it hands out:
  `+[UISDisplaySingleRectShape sb_thisDeviceDisplayShape]`, its own UIScreen's and every app's
  `UISDisplayContext.exclusionArea`.

### Changes

- `userland/inject/status-bar.c`:
  - `readDisplayWidth` (`fbwidth=`, else 1080 with `lisa=1`, else 1125);
  - `readDisplayScale` (`fbscale=`, else 3);
  - `computeSplitNotch(width, scale, notchW, notchH, referenceScale)`.
  - Test: 23 checks, among them 1080 at 3 with Split1080_Wide giving 96.48,0 167.04x35.84 and the X's 1125 at 3 with
    Split1125 giving 83,0 209x30. `run-tests.sh`: 11 files, 0 failures.
- `dlisa-inject.c`:
  - **SpringBoard:** `+[UISDisplaySingleRectShape sb_thisDeviceDisplayShape]` returns the notch of the split bar for
    the display's width, read from that class's `+notchSize` / `+referenceScreenScale`, once. It logs `status bar:
    notch 96.48,0.00 167.04x35.84 instead of SpringBoard's 70.83,0.00 178.35x25.60 (what
    _UIStatusBarVisualProvider_Split1080_Wide wants)`. UIKit in every app then picks Split1080_Wide by itself.
  - **Every program with UIKit where the library is:** `+[_UIStatusBarVisualProvider_Split
    visualProviderSubclassForScreen:visualProviderInfo:]` answers that split bar when UIKit found none. It logs `UIKit
    found no split bar for this screen; ... it is`, which should not appear once the notch matches.
  - The key of parts 3 and 4 is removed once (`... removed (saved)`).
  - The probe's "UIKit's own split bar" now calls the original method, so it shows UIKit's own pick.
  - `dlisa_sbprov=0` leaves everything alone.
- The safe areas (`sb_thisDeviceDisplayEdgeInfo`) stay type 4's. SpringBoard keeps "iPhone X Zoomed" (320-point icon
  layout); only the notch changes.

### Option C costed: present 375 x 812 points (1080 x 2340 at 2.88), the 13 mini itself

**Effect.** SpringBoard would get its exact type 0xf: the 13 mini's icon grid, safe areas and notch. UIKit would pick
Split1080_Wide with nothing changed, in every process. Every app would lay out at the 375-point width it is designed
for.

**What it takes.**
1. **Find the native size.** Find where QuartzCore/FrontBoard take a display's native size apart from its mode (the
   13 mini renders 1125x2436 and shows 1080x2340): the IOMobileFramebuffer properties CADisplay reads for `nativeSize`,
   static work in QuartzCore.
2. **Change the driver.** In `MiniIOMobileFramebuffer`, report a 1125x2436 mode with a native 1080x2340, and scale each
   frame 0.96 uniformly onto the panel with 30 black rows at the top and bottom.
3. **Pay the cost per frame.**
   - 5.7 % more pixels for the GPU;
   - the CPU scaling pass: about 3.6 ms a frame bilinear, 11 ms one in ten (the driver's own numbers), against 0 ms at
     1:1 today;
   - unless the scaling moves to the display engine's hardware scaler or the GPU, which is more driver work.
4. **Effort.** 1 to 2 sessions for 1+2 and a phone run, plus the scaler if the frame time matters; the GPU sessions'
   frame numbers would all shift.

**Recommendation.** Keep 1080x2400 1:1 with this notch fix for now. Option C is the right target once the display
scaling is in hardware.

