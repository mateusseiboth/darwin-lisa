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
