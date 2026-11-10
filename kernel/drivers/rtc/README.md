# Real-time clock (lisa, iOS 26)

Until now the calendar started at 0: no platform expert answers `PEGetUTCTimeOfDay`, and nothing in the kernel
reads the loader's `epoch=` boot-arg or its `/chosen/boot-epoch`. timed then lifted the clock to the build's time.
This driver reads the PMK8350's RTC over SPMI. Its offset to UTC persists between boots, and every time the system
sets the clock is remembered.

## How it works

- **The clock:** `qcom,pmk8350-rtc`, `rtc@6100` of the PMK8350 (SPMI slave 0, PPID 0x061) in lisa's DTB.
  The registers come from Linux `rtc-pm8xxx.c` (`pmk8350_regs`):
  - `RTC_WRITE` 0x6140..43
  - `RTC_CTRL` 0x6146 (bit 7: running)
  - `RTC_READ` 0x6148..4b, a little-endian count in seconds, read again if the low byte rolls over
- **The offset:** like every Qualcomm phone, the counter starts from an arbitrary base and is usually not writable
  by the OS. pmOS's DTB for lisa has no `allow-set-time` and no nvmem `offset` cell, so its `rtc0` is the bare
  counter. So UTC = count + offset. The offset comes from these sources, best first:
  1. boot-arg `rtc_offset=S` (`tools/rtc-offset.sh` measures it from pmOS);
  2. the offset init26 saved on the data volume, `/private/var/db/dlisa-rtc-offset`, written to
     `kern.hakios.rtc_offset` right after `/private/var` mounts. The calendar moves at once. It is refused while
     a boot-arg rules;
  3. every `settimeofday` (timed's network time, Settings): `PESetUTCTimeOfDay` -> `dlisa_rtc_set` records
     `secs - count`. init26's keeper (a child that checks every 30 s) saves it for the next boot;
  4. otherwise an estimate: the count itself if it reads as a date after 2024, else `epoch=` (the build's
     time), else the count.
- **`rtc_write=1` (opt-in):** a time set by the system is also written into the PMIC counter, the way Linux
  `__pm8xxx_rtc_set_time` does it (stop, byte 0 = 0, bytes 1..3, byte 0, start). This happens only if the arbiter
  says the channel belongs to EE 0, and the offset becomes 0. Off by default: pmOS reads the same counter.

## Files

| File | What |
| --- | --- |
| `dlisa.rtc.c` | the driver: `dlisa_rtc_get/_set`, sysctls `kern.hakios.rtc_count`, `rtc_offset` (RW), `rtc_source` |
| `dlisa.spmi.c`, `dlisa.spmi.h` | a PMIC peripheral through the SPMI arbiter v5. This is the arbiter code from `../battery/dlisa.battery.c`, made shareable. The battery driver still has its own copy; moving it onto this file is a follow-up, left alone here because another worker owns that file |
| `rtc-wire.py` | tag `hakios_rtc` (option `HAKIOS_RTC`), plus the two `#ifdef HAKIOS_RTC` hooks in `iokit/Kernel/IOPlatformExpert.cpp` |
| `../dlisa_wire.py` | the helpers the wire scripts share |
| `../../../userland/init/rtc-offset.c` (+ `test/rtc-offset-test.c`), `rtc-wifi-boot.c` | init26: apply the saved offset, keep it |
| `../../../tools/rtc-offset.sh` | `rtc_offset=N` from pmOS's `rtc0` and this PC's UTC |

Boot-args: `rtc=1` (otherwise the driver is inert: both hooks answer nothing, as before), `rtc_offset=S`,
`rtc_write=1`.

## Verified offline (2026-11-10)

- `rtc-wire.py` was run twice on a copy of the tree's files (`XNU_WORK=`): all `[ok]`, then all `[skip]`.
- Syntax-only compiles with the tree's exact flags (`tools/ksyntax.py`, clang-24 in the toolchain image), 0 errors
  for each:
  - `dlisa.rtc.c` and `dlisa.spmi.c`, using `dlisa.battery.o.json`;
  - the hooked `IOPlatformExpert.cpp`, using `IOPlatformExpert.cpo.json`. Nothing new warns near the hooks.
  - The only warnings are `-Wunsafe-buffer-usage` ones, which every driver file here gets.
- The init26 host tests pass (`sh userland/init/test/run-tests.sh`, `rtc-offset-test`), and init26 with the
  changes links with `build-init.sh`'s flags.
- Not verified, needs the phone:
  - the RTC register map;
  - whether the counter runs, and what it holds;
  - whether EE 0 owns the RTC's channel.

  The first boot logs all of these.

## Integration (combined build)

1. `python3 darwin-lisa/kernel/drivers/rtc/rtc-wire.py` (docker as root, like the other wires), then the GPU
   worker's single rebuild. It is independent of the other wires. Its only anchors are the `HAKIOS_BATTERY` options
   line, the `BSD_DEV ... BSD_EXTRAS_DEV ]` line, and `PEGet/PESetUTCTimeOfDay`.
2. `python3 darwin-lisa/userland/scripts/rtc-wifi-userland-wire.py`, then rebuild init26 (`build-init.sh`) and the
   root as usual.
3. Boot-args: add `rtc=1`. Optionally add `rtc_offset=$(tools/rtc-offset.sh)` for the first boot, measured while
   the phone is parked in pmOS.

## Device test

Log markers:

- `rtc: the PMK8350's clock: channel N (...), type 0x.. subtype 0x.., control 0x80 (running), count C s`
- `rtc: offset O s from <source>: the time is YYYY-MM-DD hh:mm:ss UTC`
- `init26: RTC the saved offset O s is the kernel's now` (from the second boot on), or
  `init26: RTC no saved offset ...`
- `rtc: the system set the time to ...`, then `init26: RTC offset O s saved for the next boot (source 5)`

Steps:

1. With the phone parked in pmOS: run `tools/rtc-offset.sh`, then `bootos.sh hakios`, and boot with `rtc=1
   rtc_offset=N` and the data volume (`var_ufs=1 ...`). The log's time matches the PC's to within about 2 s.
   About 30 s after init26, `offset ... saved`.
2. Cold boot (power off, then on, standalone) with `rtc=1` and no `rtc_offset`:
   `init26: RTC the saved offset ... is the kernel's now`.

**Pass:** the lock-screen clock shows the real time after a cold boot.

## Risks

- The PMK8350 register offsets come from memory of `rtc-pm8xxx.c`. The PPID 0x061 comes from the DTB. The arbiter
  code is the battery driver's, which is proven on this phone.
- If the counter reads `STOPPED`, `rtc_write=1` starts it, as Linux probe does.
- A dirty journal or ramfs `/private/var` means no saved offset. The clock then falls back to the estimate, no
  worse than today.
- timed may still prefer its own sources. Any `settimeofday` it makes is followed and saved.
