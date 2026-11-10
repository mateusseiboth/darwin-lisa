# Presentation build: iOS 26 boots by itself, with no PC

The goal is a phone that, from a cold power-on with no PC attached, boots the normal full iOS 26 (the full image
from `super`, the data volume, every app), reaches the home screen, and opens Settings by hand. pmOS must stay
selectable on the phone itself.

Everything here was prepared offline. Nothing has been flashed or run on the phone. It applies on top of the
data-volume work (`docs/data-volume.md`): that work's kernel, volume and root come first, and this adds three things:

1. a loader that boots the kernel it carries when no host speaks to it,
2. libdlisa fixes in SpringBoard (setup mode, activation, unlock),
3. a vendor_boot that carries the kernel and the boot-args.

## 1. Design

### The boot chain

| Step | Today | Presentation build |
|---|---|---|
| ABL | loads boot_a (U-Boot) and vendor_boot_a | the same |
| U-Boot | menu, 5 s, HakiOS. The volume keys pick pmOS | **unchanged**: boot_a is not touched |
| `hakios load` | takes the HKUB from vendor_boot's ramdisk: the loader plus a payload with only `usb` in it | the same command. The payload now also carries the kernel, the boot-args and `autoboot=10` |
| loader | waits for the host for ever | waits **10 s** for a fastboot command. With none, it runs the kernel it carries, exactly as `fastboot oem run` would |
| kernel | sent from the PC each time | from the payload. The root is in super, `/private/var` is the data volume |

The loader patch is `loader/autoboot.patch`, applied on top of `oem-bootos.patch` and `data-region.patch`:

- **A host keeps the loader.** Any fastboot command in the first 10 s cancels the auto-boot for that session.
  `bootos.sh`, `run-flashroot.sh` and `run-fullroot.sh` poll `getvar product` every 1 to 2 s, so the development
  flow works as before.
- **The run is the same as `oem run`.** The loader on core 0 feeds the hypervisor watchdog, serves the UFS mailbox
  and keeps its USB up, so a PC plugged in later can still use `fastboot oem log`, `oem screenshot`, `oem stop` and
  `reboot`.
- **When an auto-booted run ends** (the kernel returned, hung for 50 s, or never started) and no host has spoken, the
  loader restarts the phone after 10 s: menu, then HakiOS again. It never sits in the loader with nobody to talk to.
- **An older loader** ignores the `autoboot` entry and stays in host mode. Nothing breaks.

Why the payload rides in vendor_boot's ramdisk:

- The loader's payload limit is 63 MiB. The kernel is about 23 MB, and with the WiFi firmware ramdisk about 56 MB.
- ABL allows an arm64 kernel 0x5600000 bytes (86 MiB) for the kernel, the 2 MiB DTB and all ramdisks. This value
  was read from the stock `abl.elf` (LinuxLoader, `UpdateBootParams`).
- pmOS boots with a 12.9 MB vendor ramdisk, and stock Android with 21.7 MB.
- vendor_boot_a is 96 MiB.

### Keeping pmOS and the development flow

- **On the phone:** the U-Boot menu is untouched. During the 5 s countdown the volume keys pick pmOS. Holding Volume
  Down at power-on opens the menu without the countdown.
- **From the PC:** `bootos.sh pmos` works whatever the phone is doing. If iOS is running it sends `oem stop` and
  `reboot`, and the fresh loader's first command cancels the auto-boot. `run-flashroot.sh` works unchanged.
- The keyless way to U-Boot fastboot (`bootos.sh uboot`) and ABL fastboot (`abl-fastboot.sh`) are unchanged.

### The unlock: SpringBoard was in setup mode

The keystore stub was not what refused the unlock. This was found offline in the iOS 26.5.2 SpringBoard.framework,
SetupAssistant and libaks code.

`-[SBSetupManager updateInSetupMode]` turns setup mode on in two cases:

- `BYSetupAssistantNeedsToRun()` returns YES, which happens on a fresh `/private/var`;
- `SBLockdownManager` reports a bricked device: `lockdownState != 2`, because mobileactivationd cannot activate.

In setup mode SpringBoard refuses the home pans, which explains the swipe that springs back. It also refuses app
launches (`__SBWorkspaceCanLaunchApplicationWithValidScene`, `_validateRequestToOpenApplication`), and on the full
image it would start `Setup.app`. HakiOS 16.5 fixed both cases in libhakios. On iOS 26, libdlisa did nothing in
SpringBoard.

The keystore stub's answers are what libaks expects:

- lock state 0x6, which is `no_pin | been_unlocked`;
- device state `sls=3` (disabled).

The selectors the logs show as unimplemented are not on the unlock path:

| Selector | libaks function | Caller |
|---|---|---|
| 0x87 | `aks_bak_get_beacon` | SpringBoard |
| 0x6b | `aks_se_support_in_rm_is_set` | keybagd |
| 0x8e | `aks_get_internal_state` | ReportCrash |
| 0x2b | | mobileactivationd |
| 0x5d, 0x22 | | datamigrator |

So the presentation needs no kernel change for the unlock.

`userland/inject/presentation-springboard.patch` adds a SpringBoard entry to libdlisa. All the hooks are found by
name; a missing one is logged and skipped:

| What | How |
|---|---|
| never in setup mode | `-[SBSetupManager _setSetupRequiredReason:]` gets 0 (HakiOS 16.5's fix) |
| activated | `-[SBLockdownManager _setupActivationState]` sets `_state` = 2; `brickedDevice` answers NO |
| no Setup Assistant anywhere | `ForceNoBuddy = YES` in `com.apple.purplebuddy.notbackedup` (cfprefsd; SetupAssistant checks it first; kept on the data volume) |
| home screen after boot (`dlisa_unlock=1`) | from 20 s, every 5 s while still locked: `-[SBLockScreenManager lockScreenViewControllerRequestsUnlock]` (what the lock screen calls after the swipe), then `unlockUIFromSource:0x19 withOptions:nil`, last `_finishUIUnlockFromSource:withOptions:`. `=2`: again whenever it locks |
| no auto-lock (`dlisa_noidle=1`) | holds `-[SBIdleTimerGlobalCoordinator acquireIdleTimerDisableAssertionForReason:]`, the same as Auto-Lock: Never |
| slow app launches (`dlisa_watchdog=4`) | FrontBoard's launch and resume watchdog factor x4 (`-[FBApplicationProcessWatchdogPolicy _queue_watchdogScalingFactorForAppInfo:isResume:]`) |
| leaving an app | snapshots saved as JPEG or PNG, not ASTC (HakiOS 16.5's fix: the CPU ASTC encode outlasted the scene watchdog) |
| diagnostics | console lines `dlisa[SpringBoard]: unlock: ...`, `... SpringBoard wants the Setup Assistant (reason N)`, `... the device counts as activated` |

The swipe should work once setup mode is off. `dlisa_unlock=1` is the safety net: the phone reaches the home screen
by itself either way.

### The boot-args

`out/standalone-cmdline.txt` holds what `loader/build-standalone.sh` writes:

```
rd=md0 ufs=2 ufs_part=super ufs_root=1 ufs_write=1 ufs_data=super var_ufs=1 dlisa_unlock=1 dlisa_noidle=1
dlisa_watchdog=4 display=1 keystore=1 console_open=1 touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix
touch_goodix_a=1 cpus=7 cpufreq=1 cpufreq_max=2400 battery=1 wlan_stub=1 init_run=/usr/libexec/usermanagerd:--init
init_run_secs=60 sb_delay=8 bootcpu=7 loader_return_after=86400 epoch=<build time>
```

- The speed settings are today's: 7 cores at 2400 MHz. The iOS part of the demo is short, and pmOS charges the phone.
- There is no `-v` (text over the UI) and no `touch_synth` (the demo uses a real hand). The script refuses
  `touch_synth`.
- `loader_return_after=86400`: the loader feeds the watchdog for 24 h, and the kernel's own timer feeds it too.
  A hang is still caught: the loader's 50 s heartbeat check ends the run, then the phone restarts.
- **The loader cuts the boot-args at 607 characters.** It puts `debug=0x8`, `hv_wdt_pet=1`, `ufs_shared_pa=`,
  `battery_shared_pa=` and `hakios_kver=` in front, about 130 characters. The script refuses a line over 420, and
  what the kernel must see comes first.
- `epoch=` is the build time. Nothing on this path reads an RTC, so the clock shows the build time plus uptime.
  Build right before you leave, or set `PRESENT_EPOCH`.
- No WiFi: `wlan_stub=1`, which is how the driver runs booted, because wifid and the join are not done. The WiFi
  firmware ramdisk would fit (`FW_RAMDISK=userland-26/out/fw-26-pad32.apfs`, about 56 MB of payload), but it shows
  nothing in the demo and adds risk.

## 2. Files

| File | What |
|---|---|
| `loader/autoboot.patch` | the loader's auto-boot (on top of `oem-bootos.patch` + `data-region.patch`) |
| `loader/build-standalone.sh` | builds the loader, payload, HKUB and vendor_boot into `out/` (new names only); `KERNEL=`, `DATAVOL=`, `FW_RAMDISK=`, `PRESENT_EPOCH=`, `EXTRA_ARGS=`, `ARGS=` |
| `userland/inject/presentation-springboard.patch` | the SpringBoard fixes in libdlisa (not applied) |
| `tools/presentation-root.sh` | applies that patch, builds libdlisa, rebuilds `system-26.apfs.full`, checks its size |
| `out/vendor_boot-lisa-standalone.img` | **built today with the kernel26.nopac of the moment (sha 58ec7fde). Rebuild it with the final kernel (step 3)** |
| `out/loader-usb-lisa-standalone.hkub` | the same HKUB on its own, for the RAM test |

Offline checks that were run:

- The loader builds with no warnings and contains `autoboot: iOS starts in`.
- The payload walk gives `kernel, cmdline, usb, kernel.ver, autoboot=10`.
- The vendor_boot has the same header and the same DTB bytes as `vendor_boot-multiboot-lisa.img`; only
  `vendor_ramdisk_size` changes. 24,698,880 B.
- The libdlisa patch passes `git apply --check` and compiles with `-Wall` in the toolchain container, with no
  warnings.

## 3. Device steps

Do this only after the data-volume worker is done: kernel B0, the volume written (A4), and its root and loader
checks. The pmOS password comes from `PMOS_PASS`; it is never written in a file.

**Step 1: the kernel [build].** Use the data-volume kernel as it is (`docs/data-volume.md` B0: ufs + datavol + HFS).
The presentation needs no kernel patch of its own. Before going on, check it:

```sh
grep -ac 'take writes as /dev/ufs_data' research/hackios-xnu-kernel/kernel26.nopac   # 1
grep -ac hfs_mountfs research/hackios-xnu-kernel/kernel26.nopac                       # > 0
grep -ac 'MiniAppleKeyStore: AppleKeyStore stand-in' research/hackios-xnu-kernel/kernel26.nopac   # 1
```

Keep a copy with its name and sha, for example `kernel-16k-present.bak`.

**Step 2: the root [build, then WRITES: super's first 4608 MiB].**

```sh
darwin-lisa/tools/presentation-root.sh       # patch + libdlisa + FULL=1 image; refuses a size other than 4831838208
```

Then write the root as in `docs/data-volume.md` A5:

1. pmOS `dd ... count=4608 conv=notrunc,fsync`.
2. Read it back with `head -c 4831838208 | sha256sum`, which must match the new `.sha256`.
3. Run `var-write.sh verify`: the region is untouched.

This step includes the data-volume worker's new init.

**Step 3: the vendor_boot [build].**

```sh
KERNEL=research/hackios-xnu-kernel/kernel-16k-present.bak darwin-lisa/loader/build-standalone.sh
cat out/standalone-cmdline.txt
```

**Step 4: RAM test of the auto-boot, no keys [RAM-ONLY].** Nothing is flashed.

1. `tools/bootos.sh uboot` puts the phone in U-Boot's fastboot.
2. Load the new HKUB:

   ```sh
   fastboot stage out/loader-usb-lisa-standalone.hkub
   fastboot oem run:"hakios load fastboot"
   fastboot continue           # leaves fastboot_screen; U-Boot's "waiting for the host" fastboot comes up
   fastboot continue           # hakios boot: the new loader with the payload
   ```

3. Send nothing more. After 10 s the screen shows `autoboot: iOS starts in 10 s` and then the kernel. If the loader's
   USB comes up (`fastboot getvar product` = `xnuloader`), don't send anything for 10 s.
4. Once iOS is up, `fastboot oem log` and `fastboot get_staged x.log` must work, and so must `fastboot oem screenshot`.
5. Expect, in this order:
   - the UFS root and `VAR /private/var is HFS Plus from /dev/ufs_data`;
   - `dlisa[SpringBoard]: -[SBSetupManager _setSetupRequiredReason:] replaced`;
   - `... the device counts as activated`;
   - `ForceNoBuddy: set`;
   - `unlock: attempt 1` and `unlock: unlocked` (or no attempt at all, if it was never locked);
   - a home screen in the screenshot.

   A `SpringBoard wants the Setup Assistant (reason 1)` line is expected once on the first boot: it was overridden.

If the loader's USB never comes up after U-Boot's fastboot (seen once on redwood), the auto-boot still runs, because
the wait ends without a cable too. In that case watch the screen.

**Step 5: flash vendor_boot_a [WRITES: vendor_boot_a].** From pmOS (`tools/bootos.sh pmos`):

```sh
ssh mateus@172.16.42.1 'sudo dd if=/dev/disk/by-partlabel/vendor_boot_a of=/home/mateus/vendor_boot_a-before-present.img bs=1M'
scp mateus@172.16.42.1:/home/mateus/vendor_boot_a-before-present.img backups/
scp out/vendor_boot-lisa-standalone.img mateus@172.16.42.1:/tmp/
ssh mateus@172.16.42.1 'sudo dd if=/tmp/vendor_boot-lisa-standalone.img of=/dev/disk/by-partlabel/vendor_boot_a bs=1M conv=fsync &&
  sudo sh -c "echo 3 > /proc/sys/vm/drop_caches" && sudo head -c 24698880 /dev/disk/by-partlabel/vendor_boot_a | sha256sum'
sha256sum out/vendor_boot-lisa-standalone.img        # must be the same
```

The size above is today's image. Use `stat -c %s` of the one you built.

**Step 6: cold boot with no PC [the real test].**

1. Unplug the cable, hold Power until the phone is off, then press Power.
2. Expect: ABL logo, the U-Boot menu with its 5 s countdown, then the loader banner with
   `autoboot: iOS starts in 10 s`, then iOS.
3. **The first boot on a new data volume is the slow one** (the LaunchServices seeding of 252 apps, which took more
   than 18 min on ramfs). Let it finish at the desk, with the cable unplugged, so that the boot at the presentation
   is a second boot.
4. Time the second boot from power-on to the home screen, and write the time down for the demo.
5. Checks on the phone:
   - the home screen comes by itself, or a swipe up opens it;
   - a tap opens Settings, which shows its list and scrolls;
   - go back home and open Settings again;
   - the screen does not lock by itself (`dlisa_noidle`).
6. With the cable back in (a running iOS keeps the loader's USB): `fastboot oem log` and `fastboot get_staged`, then
   `grep -a 'dlisa\[SpringBoard\]\|unlock:\|Setup Assistant\|watchdog'`.
7. **pmOS from the phone:** power off, power on, press Volume Down during the countdown, pick postmarketOS. It must
   boot. Then `tools/bootos.sh hakios` brings the loader back, and a cold boot returns to iOS by itself.

Park the phone in pmOS (it charges) between tests and at the end. iOS does not charge.

### Rollback

| What | How |
|---|---|
| vendor_boot_a | `backups/vendor_boot_a-before-present.img` (step 5) through pmOS `dd`, or `fastboot flash vendor_boot_a` from ABL (`abl-fastboot.sh`, or Power + Volume Down). The datavol image (`out/vendor_boot-lisa-datavol.img`, 7c1f3182) and the multiboot one (c4d960ba) are the known ones |
| root | the previous `system-26.apfs.full` (A5 again). Or keep the new root and leave out `dlisa_*`: an unknown boot-arg does nothing |
| a single fix | the hooks are boot-arg gated except setup mode, activation, ForceNoBuddy and the snapshot format, which only stop SpringBoard from refusing to run apps. Leaving out `dlisa_unlock`, `dlisa_noidle` or `dlisa_watchdog` turns that part off |
| ForceNoBuddy | it stays on the data volume: `var-write.sh write` gives an empty volume again |

## 4. Settings: what can stop it, and what is ready

| Wall | State |
|---|---|
| setup mode refuses app launches (`__SBWorkspaceCanLaunchApplicationWithValidScene`, `_validateRequestToOpenApplication`) | **fixed** by the SpringBoard patch |
| FrontBoard launch watchdog (about 20 s for a first-party app) against a CPU-rendered first frame | `dlisa_watchdog=4` |
| scene watchdog when the app leaves the screen (ASTC snapshot on the CPU) | **fixed** (JPEG/PNG) |
| the app is not on the home screen yet | lsd registers the 252 apps on the first boot of the data volume: do that boot at the desk (step 6) |
| data container (`/var/mobile/Containers/Data/Application/...`) | containermanagerd with personas off (the existing libdlisa fix) on HFS. `clonefile` is ENOTSUP, so templates are copied, which is slower but works. Watch the log for `container` errors |
| `DataProtectionClass`: `F_SETPROTECTIONCLASS` and friends | ENOTSUP on this HFS, as on ramfs today; UIKit goes on |
| launch constraints / AMFI | `amfi-lcmatch` and miniamfi are already wired, as for SpringBoard's own apps |
| sandbox | minisandbox allows |
| daemons Settings talks to that are absent or stubbed: wifid (`wlan_stub`), bluetoothd, CommCenter, locationd, sharingd | the lookups fail fast (`error = 3`): rows show off or empty. A service that launchd knows but whose daemon crashes can block a pane until its timeout. The root list is the target of the demo, not every pane |
| memory | the full image runs about 150 processes with about 2 GB free from the flash root |
| speed | CPU rendering at about 11 frames a second (GPU 3D is not on yet): scrolling is slow but works |

Not done: Wi-Fi join, Bluetooth, cellular, the real fuel gauge.

## 5. Risks

- **Locked out away from the PC?** No. The flash changes only vendor_boot_a. U-Boot (boot_a) and its menu are as
  they are, so pmOS is always one volume-key press away at power-on. If iOS fails, the phone restarts in a loop of
  menu, then iOS; the menu is shown 5 s every time.
- **The worst case is ABL refusing the bigger vendor_boot** (not expected: 24 MB against the 86 MiB budget read from
  `abl.elf`). Then ABL stays in its fastboot mode, and recovery needs the PC: `fastboot flash vendor_boot_a` with the
  backup. Step 6 at the desk is what proves this before leaving.
- **U-Boot's `hakios load` takes a 24 MB ramdisk ABL put in memory** that U-Boot may have used since. The HKUB entries
  carry CRCs, and a damaged one is not started. U-Boot then falls back to "waiting for the host", and a long press on
  Power plus the menu gets pmOS. Step 6 shows it.
- **A PC connected at the presentation** with no script sending commands does not stop the auto-boot. Only a fastboot
  command does.
- **The clock** shows the build time (`epoch=`). Set `PRESENT_EPOCH` if it matters.
- **The battery** does not charge under iOS. Each test drains it: park the phone in pmOS.
- **The unlock fix was worked out offline.** If setup mode was not the whole story, `dlisa_unlock=1` still drives the
  unlock from inside SpringBoard, and its log lines say which step worked. If none works, add `dlisa_oslog=SpringBoard`
  to `EXTRA_ARGS` for a diagnostic boot, then grep `unlockUIFromSource`, `Ignoring unlock attempt` and
  `Device is blocked`.
- **The kernel in today's `out/vendor_boot-lisa-standalone.img`** is whatever `kernel26.nopac` was at build time.
  Rebuild with the final kernel (step 3).

## 6. After the first standalone boot (2026-11-10, afternoon)

The first cold boot with no PC reached the home screen on the full-lean root. Five problems were left. This is what
caused each one and what fixes it.

| Problem | Cause | Fix |
|---|---|---|
| "Misconfigured Device: Default Event Timing Profile is missing (D22.Default.plist)" | The loader presents the phone as iPhone10,3, D22AP (`xnuloader/identity.h`, target-type `D22`). The 23F84 image is the iPad one and ships only J817/J818/J820/J821 profiles | `plist-tweaks.py` writes `D22.Default.plist` as a copy of `J817.Default.plist`, an empty dictionary. The J8xx Touch and Pencil profiles describe 120 Hz ProMotion and Pencil hardware, so they are not copied |
| Freeze, then the lock screen | backboardd aborted (crash report on the data volume, uptime 140 s): `CA::OGL::Context::push_surface` assertion `found != slist->end ()`, reached from `CA::OGL::AsynchronousNode::retain_surface` on the display thread. An asynchronously rendering `CALayerHost` gets a surface the software context does not track. backboardd dies, SpringBoard starts again, and the phone shows the lock screen | libdlisa in SpringBoard and Preferences: `-[CALayerHost setRendersAsynchronously:]` always gets NO (`dlisa_async=1` turns this off). **Re-enable asynchronous rendering once CoreAnimation runs on the GPU** |
| "SPMI read failure." at boot | U-Boot's message (`drivers/spmi/spmi-msm.c`), not the kernel's. Each EFI Block I/O call the loader makes for the kernel runs U-Boot's timer events, and the console's event reads the volume and power keys over the PMIC arbiter. The kernel's battery ADC and touch supply use the same arbiter, and two transfers at once fail | `loader/run-tpl.patch`: the loader stays at TPL_NOTIFY while a kernel runs, so U-Boot's TPL_CALLBACK events wait until the run ends |
| Only about 5.8 KB of kernel log from a standalone boot | The kernel's copy of console output into the loader log came after the `disableConsoleOutput` check. Without `-v` the console is turned off once the boot picture is up | `kernel/patches/serial-console-tee.patch`: the copy comes before the check, so a boot without `-v` gives the whole log |
| The full image never drew a frame | SpringBoard's main thread waited for good on CommCenter: `-[SpringBoard _completeStartupAfterMainSceneConnect:]` -> `-[SBIdleTimerDescriptorFactory ...]` -> `-[SBTelephonyManager isInEmergencyCallbackMode]` -> `_CTServerConnectionGetEmergencyCallBackMode`, a synchronous XPC with retries. CommCenter runs with no baseband and never answers. The stacks were symbolized with `dyld_shared_cache_arm64e.symbols` from the SystemOS cryptex | init26 `ld_disable=` (`userland/README.md`). `com.apple.CommCenter` is in the `default` group, which is disabled at every boot. A host run with `ld_disable=CommCenter` on the full image unlocked and reached the home screen |

Symbols: `ipsw/26.5.2-extracted/23F84__iPad17,1_2_3_4/dyld_shared_cache_arm64e.symbols` (1.0 GB). It comes from the
SystemOS cryptex (`094-54821-109.dmg`, decmpfs, inflated with unzbm) and sits beside the pristine cache. To symbolize,
pass the slide that libdlisa's psample prints, or work it out from a known frame such as `__pthread_kill` + 8:

```sh
cd ipsw/26.5.2-extracted/23F84__iPad17,1_2_3_4
ipsw dyld a2s -s 0x14fd0000 dyld_shared_cache_arm64e 0x1d292b984
```

Builds: kernel `kernel-16k-present2.bak` (c54833dc), vendor_boot `out/vendor_boot-lisa-standalone.img` (abfe7629),
root `userland-26/out/system-26.apfs.full` (f2c62ac1, the full image with every fix).
