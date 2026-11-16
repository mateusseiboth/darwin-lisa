# Handover 2026-11-16: the clock in 1969 and the empty Control Center circles (host only)

Nothing was run on the phone and nothing was written to it.

## 1. The clock at epoch 0: cause and fix

**Cause.** Every boot that logs `launchd|1969-12-31 20:00:..` is a host run (gpu-quietrun.sh: poster-f1/f2/f3,
gpu-s8*, gpu-s9*, gpu-c/d/e/p*). None of those lines has `rtc=1` (nor `tz=`). Without `rtc=1` no platform clock
answers `PEGetUTCTimeOfDay`, the kernel starts the calendar at 0, and nothing reads `epoch=` (only `dlisa.rtc.c`
does, and only when `rtc=1`). Every boot with `rtc=1` on the line (the standalones: e07b4390-s7, 43cef6b7, 3d2017d2,
planb, evening-*) has the right date, with the same kernel (rc3 `1a072069` has `hakios_rtc` and the lisa defaults).
Evidence, `research/hackios-xnu-kernel/logs/`:

- `gpu-s9a-end.log` cmdline: `... epoch=1794543191 ... lisa=1 ...`, no `rtc=1`; 0 `rtc:` lines; launchd at
  `1969-12-31 20:00:31`. 20:00 is UTC-4: the data volume keeps `localtime` -> America/Campo_Grande from a standalone.
- `standalone-e07b4390-s7.log` cmdline with `rtc=1 tz=America/Campo_Grande`: `rtc: offset 1794051505 s from init26's
  saved offset`, launchd at `2026-11-12 11:28:32`.
- The Notification Center's "Wed 31 Dec" (`poster-f2-q3.png`) and Settings' status bar clock going blank at the
  minute change (Session 11) are all from host runs.

**Fix (userland, no kernel rebuild).**

- init26 `applyEpochCalendar()` (`userland/init/rtc-wifi-boot.c`), right after the saved RTC offset step: when the
  calendar is still before 2024, `settimeofday(epoch= + the seconds it has run)` (`resolveEpochCalendar()` in
  `rtc-offset.c`, host-tested). With `rtc=1` the calendar is already set and nothing changes. `mini` gained
  `settimeofday` (syscall 122; no MAC policy on this port hooks `system_check_settime`). Log line:
  `init26: CLOCK no clock set the calendar (N s): it is epoch= E + N s = T now`.
- `kernel/drivers/gpu/gpu-quietrun.sh` (commit 3db6b03) takes `epoch=` again just before `mkpayload`, so the minute
  or so `bootos.sh` takes is not missing from the clock. Same length (ten digits). run-flashroot.sh already took it
  at payload time.
- No kernel patch. A later option, not done: `rtc=1` in the lisa defaults (`lisa=1`) would give host runs the RTC and
  the saved offset too; that needs the kernel rebuilt from a tree that is rc3's, and the tree now carries the
  scheduler work.

## 2. The empty Control Center circles: what is known

The control and widget extensions all fail, in every boot with the extensions registered, whatever the clock and
whether or not the GPU is on:

- **Controls** (CalculatorWidget, LauncherControlExtension (Camera), AccessibilityControlsExtension,
  BarcodeScannerWidgetExtension; FitnessWidget too): `DLISA abort runningboardd[8] ... explanation:ChronodWidgetExtensionWatchdog`,
  `DLISA exit N(...) xstat=0x9` exactly ~10.4 s after their exec, three tries each, then chronod stops asking (no
  relaunch when Control Center opens at `cc@280`). They print nothing in those 10 s. gpu-s9a: 12 control execs,
  12 kills. Same in every gpu-*/poster-*/standalone log with `metal_all=1`, and in `vargrow-end.log` (no gpu, no
  metal): not the Metal plug-in, not the clock (standalones have the right date and the same kills).
- **Widgets** (HomeWidget, PodcastsWidget, CalendarWidgetExtension): `DLISA uxexc ... exc=6 code=0x1 sig=5` at one
  place in every boot, unslid `0x18023ee74` = `libswiftCore.dylib +0x60e74`, the `brk #1` after a `braaz x6` tail
  call to a Swift runtime override hook (the function that caches a witness table slot, likely
  swift_getAssociatedConformanceWitness). The message Swift prints before the trap went to /dev/null.
- WidgetRenderer_Default now runs and draws (earlier blocker gone). With no control template from the extensions
  it has nothing to draw: the empty circles.

Why the control extensions never check in with chronod could not be told from the logs: they are not in the
injection list, so neither their os_log nor their stderr reached the console. This root changes that.

## Built, NOT written

| Piece | What | sha256 |
|---|---|---|
| `userland-26/out/init26-clock/launchd` | init26 HEAD fda0659 (epoch fallback) | `dbeb1522f30a875b6d67593539e29fbbb1ac77138cf31bce818a29b71bae0b95` |
| `userland-26/out/libdlisa-launchprobe/libdlisa.dylib` | libdlisa c247b72: ef2a85a2 + `dlisa_launchprobe` | `141ab49bf937476262da99e61cd3b8aa728a1a48a2c6a303644b614d1774af5e` |
| `/etc/dlisa-inject.conf` | + chronod and the eight extensions | `4378bef2...` |
| root `userland-26/out/system-26.apfs.full-1682c5f3` | 2ef33580's content (sysroot + glass9 `HakiosMetal` 13ea2c9a, `Info.plist` 3769fa7f) with the three files above. Hard-link copy of `out/sysroot-26` (deleted after; the shared sysroot untouched: launchd d2506319, libdlisa f61dbf9b, conf 86a53398), `mkapfs.py -z` with `extract/system.manifest`, padded to 32 MiB. 61212 files, 24194 directories, 821 symlinks (as 2ef33580). Read back: the six files above and `DefaultWallpapers~iphone.plist` 9112d616. 4,999,610,368 B | `1682c5f35e7472877c19ad7d344a4eaa82533c6085caab0ea69d04759839a7f6` |

Both builds were checked reproducible first: HEAD's init26 rebuilds to d2506319 and HEAD's libdlisa to ef2a85a2.
Host tests: `sh userland/init/test/run-tests.sh` (5 files, 0 failures), `sh userland/inject/test/run-tests.sh`
(launch-probe-test 0 failures).

## Test run (one boot, phone in pmOS first)

```sh
cd ~/Dev/mobile && export PMOS_PASS=... SSHPASS=$PMOS_PASS
PMOS_PASS=$PMOS_PASS darwin-lisa/tools/flash-root.sh userland-26/out/system-26.apfs.full-1682c5f3   # reads back 1682c5f3
# clk-cc1: 455 characters; no GPU arguments (the controls fail without the GPU too, and the line fits)
E="dlisa_unlock=1 dlisa_noidle=1 display=1 console_open=1 lisa=1 keystore=1 init_run=/usr/libexec/usermanagerd:--init cpus=4 dlisa_oslog=chronod,CalculatorWidget,LauncherControlExtension dlisa_launchprobe=CalculatorWidget,LauncherControlExtension,HomeWidget touch_seq=nc@200,up@240,cc@280"
KERNEL=kernel-16k-consolidated-rc3.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh clk-cc1 "$E" "150 210 250 290 320"; bash darwin-lisa/tools/bootos.sh pmos
```

Pass, clock (problem 1):

- `grep -a "init26: CLOCK" clk-cc1-end.log`: `it is epoch= E + N s = T now`, T within a minute of the PC's clock at
  `### T0` (gpu-quietrun.sh with 3db6b03; with the older copy the gap is the bootos.sh time);
- `grep -a -m3 "com.apple.xpc.launchd|20" clk-cc1-end.log`: 2026 dates, local time (UTC-4);
- `clk-cc1-q2.png` (+210, Notification Center): today's date, not "Wed 31 Dec"; status bar clocks show the time.

Data for problem 2 (the run passes when these lines are there; they name the cause):

- `grep -a -E "dlisa\[(CalculatorWidget|LauncherControlExtension|HomeWidget)\]: launch probe" clk-cc1-end.log`: four
  frame lines per extension start, unslid. Symbolise with `ipsw dsc a2s` on
  `ipsw/26.5.2-extracted/23F84__iPad17,1_2_3_4/dyld_shared_cache_arm64e` (the `ipsw` binary is no longer on this host;
  the addresses above were mapped with a scratch reader of the cache's mappings, images and symtab). Blocked in a call -> that call is the cause; waiting in `mach_msg` of the run loop
  -> chronod never asked, look at chronod's side.
- `grep -a -E "oslog\[(chronod|CalculatorWidget|LauncherControlExtension)\]"`: what chronod asks the extension
  and what the extension says before the 10 s kill.
- `grep -a -i "Fatal error" clk-cc1-end.log`: the Swift message behind HomeWidget's / PodcastsWidget's trap.
- `clk-cc1-q4.png` (+290, Control Center): expected still empty circles (nothing fixed there yet).

Rollback: `tools/flash-root.sh userland-26/out/system-26.apfs.full-2ef33580` (only the three files differ).
