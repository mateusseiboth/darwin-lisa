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
| 3. Sensors through the SSC | Kernel QMI/protobuf client + iOS rotation hook done and host-tested |
| 4. The sensors PD's FastRPC file server (`sensors_rpc=1`) | Written 2026-11-10 evening, offline: protocol + file server host-tested (237 checks), kernel side syntax- and link-checked, staging + wire scripts tested. NOT in final1; needs a final2 build and has never run on the phone (see "Sensors: FastRPC") |

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
| `dlisa.fastrpc_proto.h` | FastRPC's wire formats (invoke payload, message, response, the listener's packing) and the file server the sensors PD reads (remotectl, apps_std, apps_mem, adspmsgd_apps) over the linked-in archive with a RAM overlay for writes. Pure C, host-tested |
| `dlisa.fastrpc.c` | The kernel's side (`sensors_rpc=1`): an SMMU context for stream 0x1805, the payload memory, attach to the sensors PD, the listener thread; sysctl `kern.hakios.sensors_rpc` |
| `dlisa.fastrpc.h` | Its interface with `hakios.adsp.c`. Present in the tree (sensors-wire.py) = the hooks are compiled in |
| `fw-sensors-stage.py` | `stage` (vendor `etc/sensors` + registry + socinfo into `research/hackios-xnu-kernel/fw-sensors/`, git-ignored, packed as `sensors.fs`), `check`, `header` (the `.incbin` header), `dump-persist` (the phone's persist `sensors/`, read-only, from pmOS over SSH) |
| `sensors-wire.py` | After `adsp-wire.py`: copies the three files and `hakios.adsp.c`, adds `dlisa.fastrpc.c` under `hakios_adsp`, generates `bsd/dev/dlisa.sensors_fs.h`. Idempotent, `XNU_WORK=` for a dry run |
| `test/run-fastrpc-tests.sh` | Host tests: `fastrpc-test.c` (the proto, gcc+UBSan, clang ASan+UBSan, valgrind), `sensors-stage-test.sh` + `sensors-archive-test.c` (staging, and the archive read back through the server) |

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
| `sensors_rpc=1` | The sensors PD's FastRPC file server (dlisa.fastrpc.c): answer the DSP's channel `fastrpcglink-apps-dsp`, attach, serve the files. `sensors_rpc_sid=3/4/5` (default 5, the SMMU stream 0x1800+N), `sensors_rpc_wait=S` (60, for the channel), `sensors_rpc_tries=N` (30 attaches, 2 s apart), `sensors_rpc_trace=N` (400 file calls in the log) |
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

That was the state before 2026-11-10 evening. The next section is what replaced it.

## Sensors: FastRPC (how Android serves the sensors PD on this vendor, and what this kernel does instead)

### Findings (read from the stock OS2.0.8.0 vendor, its NON-HLOS/persist images, and the references)

- **The firmware matches the vendor.** pmOS's `adsp.mbn` (what `fw-adsp/` holds) and the stock package's
  `NON-HLOS.bin:/image/adsp.*` are the same build, `ADSP.HT.5.5-00832.1-LAHAINA-1_20210510`; the only differences are
  the signing stamps (`OEM_IMAGE_UUID_STRING`, 2024-06-05 vs 2025-08-18) in b21 and the hash segment. The sensor
  drivers are built in (b21: `sns_icm4x6xx`, `sns_icm4x6xa`, `sns_ak0991x`, `sns_stk3a6x`, `sns_registry_parser`, ...).
  `vendor/dsp` (dspso.bin) has no sensor library: nothing is loaded from the host for the sensors.
- **Who serves it on Android:** `vendor.sensors` = `/vendor/bin/sscrpcd sensorspd` (class early_hal). sscrpcd dlopens
  `libssc_default_listener.so` if `/sys/kernel/boot_slpi` exists, else `libadsp_default_listener.so` (lisa: the latter,
  no SLPI), and calls `adsp_default_listener_start("sensorspd")`: libadsprpc opens `createstaticpd:sensorspd` on the
  ADSP, which is `FASTRPC_INIT_ATTACH_SENSORS` in the vendor's adsprpc driver: an invoke of handle 1 method 0
  (`FASTRPC_RMID_INIT_ATTACH`) with the tgid, sent with pid 0 and `fl->pd = 2`, so every later message's ctx ends in 2
  and goes to the sensors PD. Then `remotectl open "adsp_default_listener"` + its `register()`, the listener's
  `init2()` (handle 3, method 3) and the `next2()` loop (method 4; `get_in_bufs2`, method 5, for large requests).
- **The channel and the SMMU:** GLINK channel `fastrpcglink-apps-dsp` on the ADSP (both the vendor DT and mainline's
  sc7280.dtsi). The vendor DT (`vendor_boot.img`, dtb "Yupik") gives the ADSP's FastRPC context banks as
  `qcom,msm_fastrpc_compute_cb10..12` = streams **0x1803, 0x1804, 0x1805** (mask 0), IOVA pool 0x80000000 + 0x78000000,
  and cb12 (0x1805) `shared-cb`: libadsprpc asks for the shared context bank for the sensors PD
  (`DSPRPC_SMMU_SUPPORT sharedcb=1`), so on Android the sensors PD's buffers go through **0x1805**. The DSP is given
  addresses with the context bank's number in bits 32..35 (`buf->phys += cb << 32`): 5 << 32 here.
- **The protocol version:** the classic one both drivers share: `struct fastrpc_msg` (pid, tid, ctx, handle, sc, addr,
  size; 40 bytes), the payload of `fastrpc_get_args()` (remote args, invoke bufs, phy pages, fdlist 16, crclist 64, poll
  word, then the arguments 128-aligned), responses `{ctx, retval}` or the vendor's v2 `{ctx, retval, flags,
  early_wake_time, version}`. The reverse calls arrive packed as `listener_buf.h` packs them.
- **What the PD calls back** (its strings in b21, the IDLs, hexagonrpcd's tables): `apps_std` — `fopen`/
  `fopen_with_env(ADSP_LIBRARY_PATH)`, `fread`, `fseek`, `flen`, `fclose`, `opendir`/`readdir`/`closedir` (the registry
  directory), `stat`, `fileExists`, and for the registry's writes `fwrite`, `frename` (`rename error %i`), `fremove`,
  `mkdir`; `apps_mem` `request_map64` (pages for its heap); `print_string`.
- **The files it opens:** `/vendor/etc/sensors/sns_reg_config` (version 6; lists `/sys/devices/soc0/{hw_platform,
  platform_subtype,platform_subtype_id,platform_version,soc_id,revision}` and the output
  `/mnt/vendor/persist/sensors/registry/registry`), the 57 JSON of `/vendor/etc/sensors/config` (each with a `config`
  header of hw_platform and soc_id: lisa's are `QRD`/`LISA` and 475), the registry directory, `sns_reg_version`,
  `sensors_list.txt`, `temp.json`, the property files `/mnt/vendor/persist/sensors/registry/file1|file2`, and Xiaomi's
  panel files for the light sensor (`/sys/class/backlight/panel0-backlight/brightness`,
  `/sys/class/mi_display/disp-DSI-0/{panel_info,dynamic_fps}`).
- **The registry:** the stock package's `persist.img` is the factory one: `sensors/registry/registry/sensors_registry`
  is empty and `sns_reg_version` says 0, so on a new phone the DSP builds the registry from the JSON and writes it to
  persist. A used phone's persist holds the built registry with its calibration (`*.fac_cal`). pmOS's
  `firmware-xiaomi-lisa` package ships such a registry (250 files, 112 KB, another lisa's) and socinfo values (`QRD`,
  475, `65536`, `1.0`) for hexagonrpcd's layout: they are the default source here.

### What this kernel does (dlisa.fastrpc.c + dlisa.fastrpc_proto.h)

The same as sscrpcd + libadsprpc + the adsprpc driver, in one kernel thread, started after the link and before the
SSC client:

1. answers the DSP's channel `fastrpcglink-apps-dsp` (only with `sensors_rpc=1`; otherwise it stays unanswered, as
   in final1);
2. takes a free SMMU match register and context for stream 0x1805 under `hakios_smmu_lock`, with our page tables: 1 MB
   of payload at IOVA 0x80000000 and room for 31 MB of `apps_mem` heap pages behind it (the Wi-Fi's proven way);
3. attach (pid 0, PD 2), `remotectl open "adsp_default_listener"`, `register`, close, `init2`; 30 tries 2 s apart;
4. the `next2` loop, serving each reverse call from the archive linked into the kernel (`fw-sensors-stage.py`: 317
   files, 292 KB: 57 JSON + `sns_reg_config`, the registry, socinfo). Writes go to an overlay in RAM (4 MB at most):
   **persist is never written**. Paths are normalised, Android's aliases folded (`/system/vendor`, `/persist`), `..`
   refused; writes only under `/mnt/vendor/persist` and `/data/vendor/sensors`;
5. `dlisa.ssc.c` then finds service 400 as before, and now also enables sensors told of late (the registry is read
   meanwhile): the missing types are asked again every 10 s for two minutes.

Sizes: dlisa.fastrpc_proto.h ~1,700 lines, dlisa.fastrpc.c ~800, hakios.adsp.c +70, dlisa.ssc.c +60; the kernel grows
by ~0.5 MB (the archive is 299,113 bytes); nothing in the root.

### Integration (final2)

1. `python3 -I kernel/drivers/adsp/fw-sensors-stage.py stage` (done: `research/hackios-xnu-kernel/fw-sensors/`, the
   pmOS registry). For this phone's own calibration: `dump-persist` first (pmOS, read-only), then `stage`.
2. In the toolchain container, as root, after battery-wire.py and adsp-wire.py (and the other final1 wires):
   `python3 -I kernel/drivers/adsp/sensors-wire.py` (twice: the second all `[skip]`), the rebuild, `grep error:`.
   Check `llvm-nm` for `_dlisa_fastrpc_start` and `_dlisa_sensors_fs`.
3. The root needs nothing new (`libdlisa-motion.dylib` is in final1's root already).
4. Device test: `docs/evening-session.md`, "Boot 5 (OPTIONAL)".

### Verified offline (2026-11-10)

- `test/run-fastrpc-tests.sh`: 237 checks of the proto (payload bytes, messages, responses, packing, archive
  validation, every apps_std/remotectl/apps_mem method, the overlay, refusals), clean under gcc+UBSan, clang
  ASan+UBSan and valgrind; the staging test; the real 317-file archive read back file by file and directory by
  directory through the server (`sensors-archive-test`).
- `test/run-tests.sh` (dlisa.ssc.c) passes with the new late-sensor test.
- `check-syntax.sh`: 0 errors in hakios.adsp.c, dlisa.ssc.c, dlisa.fastrpc.c, the battery files; hakios.adsp.c also
  without dlisa.fastrpc.h (the hooks compiled out), dlisa.fastrpc.c also with the generated archive header.
- Objects built with the tree's exact flags (archive assembled in): every undefined symbol of dlisa.fastrpc.o and the
  new hakios.adsp.o is defined by the current kernel or by them; dlisa.fastrpc's globals collide with nothing.
- `sensors-wire.py` dry-run twice on a copy of the tree's config/conf files: first applies, second skips all.

Not verifiable offline: whether the hypervisor grants stream 0x1805 a context (it granted the Wi-Fi's 0x1c00), the
attach's answer, the exact calls the PD makes, its heap requests.

### Still to do

- iOS light and proximity: virtual HID services in backboardd (an ALS service for CoreBrightness, a proximity
  service for `BKHIDProximityEventProcessor`), about 300 lines in `libdlisa.dylib`'s way. Not started: it waits for real
  values in `kern.hakios.sensors` (Boot 5 shows whether they come).
- Apps' CoreMotion (raw accelerometer, gyro) needs locationd or a CoreMotion hook per app: out of scope.
- The overlay is RAM only: a registry the DSP writes is rebuilt every boot (seconds). Keeping it on the data volume
  is possible later; persist stays untouched either way.

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
- FastRPC (`sensors_rpc=1`, final2 only): the SMMU context is written once per boot and put back on stop and on the
  panic path. If the DSP faults on our memory the SMMU records it (stall-disable, as the Wi-Fi's); the worst case is
  the sensors PD crashing, which can take the ADSP down with it (and the battery gauge). The default boot-args do not
  include `sensors_rpc=1`, so Boots 1 to 4 are unaffected even with a final2 kernel.
- The registry is another lisa's unless `dump-persist` ran: accelerometer/gyro offsets of a few hundredths of g, the
  light sensor's calibration off. The orientation does not care.
