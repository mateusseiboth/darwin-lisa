# ADSP on iOS 26 (lisa): boot, fuel gauge and charging, sensors

The audio DSP (ADSP, LPASS Hexagon) runs three things iOS needs on lisa: the battery manager (fuel gauge and charger,
Qualcomm's pmic_glink "battmgr"), the audio services, and the Snapdragon Sensor Core (accelerometer, gyroscope,
magnetometer, light, proximity). Nothing of it runs until the host loads the DSP's firmware. This directory boots it
and talks to two of the three.

Status (2026-11-10, offline only; nothing here has run on the phone yet):

| Item | State |
| --- | --- |
| 1. ADSP boot: PIL through TrustZone, SMP2P, GLINK over SMEM, QRTR | Ported from HakiOS (proven on redwood, the same SM7325). lisa's DT has every address unchanged. Compiles with the 26 build's flags, links against the current kernel. Firmware staged |
| 2. battmgr: fuel gauge, health, cycles, current with lisa's sign, charger status | Done in code, feeds HakiosBattery. Charging needs no host command (see "Charging") |
| 3. Sensors through the SSC | Kernel QMI/protobuf client + iOS rotation hook done and host-tested. The sensor core will most likely NOT give physical sensors without a FastRPC file server (see "Sensors"). `sensors_fake=2` tests the iOS rotation path in the same boot either way |

## Files

| File | What |
| --- | --- |
| `hakios.adsp.c` | HakiOS `drivers/audio/hakios.adsp.c` with the darwin-lisa changes listed in its header: battmgr opt-in (`battmgr=1`), battmgr no longer waits for APR, the DSP is kept once GLINK is up, lisa's current sign, health, Linux's notification request, our own QRTR ports for the sensors client, `vm_kern_xnu.h` for xnu-12377 |
| `dlisa.ssc.c` | The sensor core client (QMI service 400 over the DSP's QRTR, protobuf payloads), `sensors_fake`, sysctls `kern.hakios.sensors` (binary) and `kern.hakios.sensors_state` (text) |
| `adsp-wire.py` | Wires both into xnu-work as tag `hakios_adsp`, re-copies the two battery files, and builds the firmware into the kernel (step 5: `bsd/dev/hakios.adsp_fw/` + `hakios.adsp_fw.h`, generated, never in git; `FW_ADSP=` empty leaves it out). Idempotent, `XNU_WORK=` for a dry run |
| `fw-adsp-stage.py` | `stage` (pmOS `adsp.mbn` split into `adsp.mdt` + `adsp.b02..b36` in `research/hackios-xnu-kernel/fw-adsp/`, git-ignored), `check DIR` (the driver's own checks), `install DIR DEST`, `header DIR OUT` (the `.incbin` header the kernel build takes; host test `test/fw-builtin-test.sh`) |
| `install-root.sh SYSROOT` | Puts `libdlisa-motion.dylib` in `SYSROOT/usr/local/lib/`, and the inject line in `SYSROOT/private/etc/dlisa-inject.conf` |
| `check-syntax.sh` | `-fsyntax-only` of the four kernel files with the exact flags of the last build (`bsd/DEVELOPMENT/.CFLAGS`, `iokit/DEVELOPMENT/.CXXFLAGS`) in the toolchain image |
| `test/run-tests.sh` | Host test of `dlisa.ssc.c`'s wire formats (the request bytes, reports, packed/unpacked floats, rotation, proximity, truncated input) |
| `../battery/dlisa.battery.c` | Now also the sink of the fuel gauge: the ADC estimate is not passed on while a DSP report is less than 60 s old; new sysctls `battery_source`, `battery_current_ma`, `battery_temp_tenths`, `battery_status`, `battery_health`, `battery_cycles` |
| `../battery/hakios.battery.cpp` | HakiosBattery now sets `BatteryHealth` (Good/Fair/Poor) and `ChargeStatus` (too hot / too cold) from the gauge's health |
| `../../../userland/motion/` | `dlisa-motion.c` + `build-motion.sh`: backboardd's device orientation from `kern.hakios.sensors` |

## Where the firmware lives (2026-11-10, the evening integration)

In the kernel. The root had about 6 MB free before the data volume that starts right after it in `super`, and the
firmware is 20.8 MB (10.5 MB compressed in the root), so `adsp-wire.py` assembles `adsp.mdt` and the 35 segments into
the kernel's `__TEXT,__const` with `.incbin` (each on a 4 KB boundary). `image_file()` still asks the RAM disks and the
flash root's `fw/` first, so a firmware RAM disk overrides the built-in set; the log then says which. The kernel grows
from 23.4 to about 45 MB: the loader's payload holds 63 MiB, and the kernel's extent stays under the resident RAM
disk's place (128 MB above the virtual base). A side effect: `tools/wifi-run.sh`, which copies the root's `fw/` into a
32 MiB RAM disk, keeps fitting.

## Boot-args

All inert unless given.

| Arg | Effect |
| --- | --- |
| `adsp=1` | Boot the ADSP on a thread beside the system's start (waits for the root, reads `/fw/adsp.*`), link GLINK and QRTR, keep it up |
| `battmgr=1` | Poll the fuel gauge every 10 s (at once after a DSP notice) into HakiosBattery. `battmgr=2`: the same plus every battery (0..31) and USB (0..11) property once in the log |
| `battmgr_sign=1` | Keep the firmware's current sign. Default `-1`: lisa reports negative while charging |
| `sensors=1` | Start the SSC client once the DSP is linked (`sensors_wait=S` default 90, `sensors_rate=Hz` 5/10/25/50/100, `sensors_rot=90/180/270`) |
| `sensors_fake=1` | Made-up gravity turning a quarter every 15 s (`sensors_fake_secs=N`), no DSP needed |
| `sensors_fake=2` | The same, but only if no real accelerometer event came within `sensors_wait`+60 s |
| HakiOS's | `adsp_step=N` (steps 1..N, one by one, for debugging), `adsp_wait=S`, `adsp_listen=S`, `adsp_again=1`, `adsp_keep=1` |

`battery=1` (the SPMI ADC estimate) can stay on: it steps aside while the gauge reports, and takes over again if the
gauge stops.

## Verified offline

- `fw-adsp-stage.py stage`: pmOS lisa `adsp.mbn` (20,846,348 bytes) splits into `adsp.mdt` (9,660 bytes = 52 + 38*32 +
  8,392 of hashes, under the 16 KB page) + 35 segments; every loadable segment inside 0x86700000 + 64 MB, the lowest
  exactly there, the image exactly 0x4000000. Passes the driver's step-2 checks.
- lisa's DT (pmOS `sm7325-xiaomi-lisa.dtb`, decompiled): `adsp_mem` 0x86700000/0x4000000, `qcom,sc7280-adsp-pas`,
  `smp2p-adsp` `qcom,smem = <443 429>`, remote pid 2, IPCC client 3 (signal 0 GLINK, 2 SMP2P), SMEM 0x80900000/2 MB,
  TCSR mutex 0x1f40000 lock 3, AOSS QMP 0xc300000. All equal to HakiOS's redwood numbers.
- `check-syntax.sh`: 0 errors in all four files (only the toolchain's `-Woverriding-option` and the IOKit header
  warnings every C++ file here has).
- Link check: the four files compiled to objects; every undefined symbol is defined by the current kernel
  (`mach.development.bcm2837.unstripped.noctf`) or by these objects; the only global collisions are
  `hakios_adsp_init/_stop/_quiesce`, which the tree's `hakios.stubs.c` drops under `HAKIOS_ADSP`.
- `adsp-wire.py` dry-run twice on a copy of the tree's config and conf files: first run applies, second skips all.
- `test/run-tests.sh`: passes. The protobuf field numbers were read from pmOS's `libssc.so.2` protobuf-c descriptors
  (SscClientRequest {1 uid, 2 msg_id fixed32, 3 config {1 processor, 2 suspend_mode}, 4 request {2 msg}},
  SscSuidRequest {1 data_type, 2 enable_updates}, SscClientResponse {1 uid, 2 response {1 msg_id, 2 timestamp, 3 msg}},
  sensor events {1 repeated float, 2 accuracy}, proximity {1 near, 2 distance}).
- `userland/motion/build-motion.sh`: builds an arm64e dylib; `install-root.sh` run twice on a scratch root: idempotent.

Not verified offline (from memory, logged on the phone): the QMI message ids 0x20/0x21/0x22 and TLV numbers of the
SSC service (libqmi's), the SSC message ids 512/513/514/768/1025, the battmgr notification request (Linux's
`qcom_battmgr`), whether `CMDeviceOrientation` has `-initWithDeviceOrientation:` (the hook falls back to a subclass).

## Integration

1. `python3 -I kernel/drivers/adsp/fw-adsp-stage.py stage` (already done once; `check` to confirm).
2. In the toolchain container, as root, after `kernel/drivers/battery/battery-wire.py`:
   `python3 -I kernel/drivers/adsp/adsp-wire.py`, then the one combined rebuild. Grep `logs/kernel.log` for `error:`.
   Check with `llvm-nm` that `_hakios_adsp_init`, `_dlisa_ssc_start` and `_dlisa_battery_note_health` are in the kernel.
3. `sh userland/motion/build-motion.sh` (built already), then, after `userland/inject/build-inject.sh` (it rewrites
   the inject conf): `sh kernel/drivers/adsp/install-root.sh ~/Dev/mobile/userland-26/out/sysroot-26`, then re-image the
   root. The firmware is NOT put in the root any more (it is in the kernel, see below); `ADSP_FW_ROOT=1` puts it in
   `SYSROOT/fw/` as before.
4. Boot-args for the combined session: `adsp=1 battmgr=2 sensors=1 sensors_fake=2` (keep `battery=1`).

## Device test plan

Before: park in pmOS and note, within a minute of rebooting to iOS, from
`/sys/class/power_supply/qcom-battmgr-bat/`: `capacity`, `voltage_now`, `current_now`, `temp`, `status`, `health`,
`cycle_count`, `charge_full`, `charge_full_design`; and `qcom-battmgr-usb/online`, `usb_type`.

### ADSP boot

Log markers, in order: `adsp: the audio DSP is being brought up`, `adsp: 7: ... slave-kernel` turning to `0x6`,
`THE CHANNEL "IPCRTR" IS OPEN`, `THE CHANNEL "PMIC_RTR_ADSP_APPS" IS OPEN`, `adsp: qrtr: HELLO FROM NODE`, a list of
`adsp: qrtr: a service: ...`, then `THE AUDIO DSP IS UP AND LINKED`.

Pass: `UP AND LINKED` within 60 s of the root mount, no panic, WiFi still scans (both drivers share IPCC, AOSS and RPMh).
Failure to note: the step number in `THE AUDIO DSP DID NOT COME UP: step N failed`, and `THE AUDIO DSP'S LAST WORDS`.

### "% matches pmOS"

`battmgr=2` prints every property once. Check the numbering first: property 4 is the capacity (hundredths of a
percent), 7 the voltage (uV), 9 the current, 13 the temperature (hundredths of a degree), 16 the cycle count. HakiOS
found these on redwood's firmware (Linux's order up to 9, then one extra property); if lisa's dump shows the
temperature at 12, change `BATT_TEMP` and the four after it.

Pass: the `adsp: battmgr: the DSP says:` capacity within 2 percentage points of pmOS's `capacity`, cycles equal,
temperature within 3 C, status bar percentage equals the logged capacity, `kern.hakios.battery_source` = 2,
`HakiosBattery: the battery's power source is up` with the gauge's percentage.

### "Charging while iOS runs: VBAT rises / IsCharging true"

Use a wall charger (DCP, QC or PD). A PC port is SDP, 500 mA, and the phone at 7 cores and 2.4 GHz draws more than
that, so VBAT can fall on a PC port even when charging works.

Pass, over 10 minutes on the wall charger: `USB online 1`, `type` 2 or more, `status 1`, the reported current
positive and above 300 mA (the firmware's own sign negative, as on pmOS), capacity and VBAT rising, the lock screen
shows "Charging" and the status bar the bolt (`IsCharging` true). If the firmware says status 2 while the current is
positive, the log says `reported as charging`. Unplug: within 1 s a notice (`< a notice`), `USB online 0`, status 2.

### "Rotate the phone -> the UI rotates" (or a sensor values log)

`sensors=1` lines: `ssc: the sensor core is QMI service 400 ...` or `ssc: NO SENSOR CORE`; then one
`ssc: the sensor core has N sensors of type "..."` per type, and `ssc: service 400 ...; SUIDs: ...`. Every 10 s:
`ssc: events: accel N ...`.

Pass A (real sensors): accel events counting, gravity about 9800 mm/s2 on the axis that points up; rotating the phone
in Calculator or Safari (Control Center's orientation lock off) rotates the UI; `dlisa-motion: the device is now ...`.

Pass B (fallback, expected): `ssc: sensors_fake=2: no accelerometer event ...`, then Calculator turns every 15 s, with
`dlisa-motion:` lines in the console. This proves the iOS side; the sensor core's own lines say what it lacks.

## Charging: does the host have to enable it?

No command beyond what this sends. pmOS charges on lisa with Linux's `qcom_battmgr` + `pmic_glink`, which send exactly
one thing besides property reads: the notification request (owner 32778, type 2, opcode 4, four zero words). This
driver sends the same. HakiOS on redwood (no request at all) already read "charging" and matched Android. The ADSP's
charger PD runs input detection (BC1.2, QC, PD) and the charge loop by itself once the DSP is up. What the host
controls on iOS is the load: at 7 cores and 2.4 GHz on an SDP port the battery can still fall.

## Sensors: what iOS needs and what is missing

The iOS side, read from backboardd and the 23F84 cache: backboardd's `BKOrientationManager` gets device orientation
from CoreMotion, `-[CMDeviceOrientationManager startDeviceOrientationUpdatesToQueue:withHandler:]`. On an iPhone
CoreMotion gets it from locationd, which reads the always-on processor's HID services (AppleSPU). lisa has no SPU and
the lean image drops locationd. So `libdlisa-motion.dylib` replaces that manager inside backboardd only and feeds it
from `kern.hakios.sensors`. That is the minimum for rotation; no HID service, no locationd. Light and proximity would
need virtual HID services in backboardd (the touch screen's way in `libdlisa.dylib`: an ALS service for CoreBrightness,
a proximity service for `BKHIDProximityEventProcessor`), about 300 lines once real values exist. Apps' CoreMotion
(raw accelerometer, gyro) would need locationd or a CoreMotion hook in each app: out of scope.

HakiOS 16.5 had no sensors at all.

The DSP side is the hard part. pmOS has `libssc` and an `iio-sensor-proxy` built with it, but its lisa image does not
ship `hexagonrpcd`, and libssc says why that matters: `'registry' sensor unavailable, is hexagonrpcd running?`. The
sensors PD on the ADSP reads its registry and the drivers' JSON (`/usr/share/qcom/sensors`, 250 files: ICM-4x6xx,
AK0991x, STK3A6x, SX932x/SX933x) from the host through FastRPC's file service. Without it the physical sensors are not
configured. Sensors do not work in pmOS on lisa either.

What a working accelerometer needs, with sizes:

| Piece | Size |
| --- | --- |
| GLINK channel `fastrpcglink-apps-dsp` in `hakios.adsp.c` | small, the channel table already takes more |
| An apps-SMMU context for stream 0x1803 (the DT's compute-cb 3), so the DSP reads our buffers. HakiOS `hakios.sound.c` does this for 0x1801 | about 300 lines, reusable |
| FastRPC invoke and reverse-invoke (the listener) protocol in the kernel | about 600 lines |
| Attach to the sensors PD, serve `apps_std` (fopen/fread/fseek/fclose/opendir/stat) from `/fw/sensors/` | about 800 lines |
| Stage the registry and config files | small |

About 1,700 to 2,000 lines and one to two weeks with several device sessions. The unknowns: whether the hypervisor's
stage 2 lets the DSP's stream 0x1803 reach our memory (it did for 0x1801 on redwood), and the FastRPC version lisa's
firmware speaks. Once that exists, `dlisa.ssc.c` and the rotation hook work as they are.

## Risks

- The ADSP has never run under this kernel. HakiOS's code ran on redwood with its own firmware; lisa's firmware is a
  different Xiaomi build. Failures stop at a named step, and the DSP is shut down if it does not link.
- Only a hard reset stops a DSP left running (there is no orderly loader return in darwin-lisa). Every lisa reboot is a
  PSHOLD hard reset, so pmOS starts clean.
- battmgr property numbers beyond 9 are redwood's; `battmgr=2` checks them on the first boot.
- The current sign default (`-1`) is from the pmOS note. If the log shows the firmware positive while charging, use
  `battmgr_sign=1`.
- The SSC QMI ids and TLVs are from libqmi from memory; the first six messages are dumped in hex.
- `sensors_fake=2` rotates rotatable apps every 15 s after about 2.5 minutes if the real sensors do not work. The
  lock and home screens are portrait-only on iPhone, so the GPU and WiFi tests in the same session are not affected.
- 20.8 MB more in the kernel (45 MB, under the payload's 63 MiB), none in the root image.
