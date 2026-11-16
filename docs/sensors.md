# Sensors on darwin-lisa (SSC on the ADSP to iOS HID)

State on 2026-11-16 (host only; nothing was run on the phone in this round).

## What lisa has

From the registry and the vendor JSON (`research/hackios-xnu-kernel/fw-sensors/`, `fw-sensors-stage.py`):

| Sensor | Part (SSC driver) | iOS consumer |
| --- | --- | --- |
| accelerometer + gyroscope | TDK ICM-4x6xx or ICM-4x6xA (`icm4x6xx_0`, `icm4x6xa_0`; one is fitted) | rotation, UIAccelerometer |
| magnetometer | AKM AK0991x (`ak0991x_0`) | compass (only via locationd) |
| ambient light + proximity (front) | Sensortek STK3A6x or AMS TCS3701 (`stk3a6x_0`, `tcs3701_new`) | CoreBrightness, proximity processor |
| ambient light (rear) | AMS TSL2540 (`tsl2540_rear`) | (CoreBrightness has a rear ALS module, not wired here) |
| SAR | Semtech SX932x / SX933x | none |
| virtual (on the DSP) | device_orient, rotv, geomag_rv, gravity, pedometer, smd, tilt, ... | none yet |

## The chain

```
ICM/AK/STK on the ADSP's sensor island
  -> SSC (QMI service 400 over QRTR)            kernel/drivers/adsp/dlisa.ssc.c        sensors=1
  <- registry + JSON over FastRPC (sensors PD)  kernel/drivers/adsp/dlisa.fastrpc.c    sensors_rpc=1
  -> sysctl kern.hakios.sensors (struct dlisa_sensors)
  -> backboardd: libdlisa-motion.dylib
       dlisa-motion.c   CMDeviceOrientationManager replaced: device orientation -> BKOrientationManager (rotation)
       sensors-hid.c    built-in virtual HID services: accelerometer, gyroscope, compass, ambient light, proximity
```

### Where it stood before this round (device logs `research/hackios-xnu-kernel/logs/evening-b1b*.log`)

The ADSP boots and links (12 QRTR services, service 400 among them). The SSC client talks to it: every SUID lookup
answers, but with no SUID for accel, gyro, mag, light, proximity, or even `registry`. The physical drivers need the
registry and the JSON, which the sensors PD reads from the host over FastRPC, and that server (`sensors_rpc=1`) had
never been in a kernel that ran on the phone: `kernel-16k-final2.bak` had it but Boot 5 never ran, and every later
kernel (handover 11-11 on) left it out.

Rotation itself was proven with `sensors_fake=1` (evening-b1b: `dlisa-motion: the device is now landscape ...`), but
the inject conf lost the `libdlisa-motion.dylib backboardd` line when `userland/inject/build-inject.sh` rewrote it, so
roots after final1 do not load the hook at all.

### The FastRPC side, checked against hexagonrpcd

`linux-msm/hexagonrpc` (hexagonrpcd -s) does the same steps as `attach_listener()`: attach to the sensors PD,
`remotectl open "adsp_default_listener"`, method 0 (register), then the listener's `init2` (handle 3, method 3) and
the `next2` loop (method 4), extended method ids (31: the real id in the first input buffer), reverse handles
0 remotectl, then the interfaces it opens. No difference found. What only the phone can say: whether stream 0x1805
gets an SMMU context, the attach's answer, the PD's file calls.

## What iOS 26 looks for (23F84, read from backboardd and the dyld cache)

Tools: `objc.py`/`annot.py` (ObjC methods and `objc_msgSend$sel` stubs of a standalone Mach-O on an llvm-objdump
listing) and `dsc.py` (split dyld cache: images, local symbols, C/CF strings, xrefs, ELF of an image's text for
llvm-objdump). They are scratch tools, not in git; `.work/bbd.dis` is the backboardd listing.

backboardd, `BKIOHIDServiceMatcher -initWithUsagePage:usage:builtIn:dataProvider:` call sites:

| Page, usage, built-in | Who | Notes |
| --- | --- | --- |
| 0xff00, 0x03, yes | `-[BKOrientationManager _initWithDeviceOrientationManager:]` | the accelerometer service |
| 0xff00, 0x09, yes | `-[BKHIDStudyLogEventProcessor initWithContext:]` | gyroscope (study log only) |
| 0xff00, 0x0b, no | `-[BKSmartCoverHIDEventProcessor initWithContext:]` | smart cover |
| 0xff00, 0x66, yes | `-[BKHIDTouchSensitiveButtonScanningController initWithContext:]` | touch-sensitive button |
| 0x20, 0x11 / 0x12, no | `-[BKHumanPresenceHIDEventProcessor init]` | human presence (not published) |

- Accelerometer events (type 13: x, y, z as floats, then an integer) go to `-[BKAccelerometerInterface
  processEvent:sender:dispatcher:]`, which hands them to UIAccelerometer/BKSAccelerometer clients and logs
  `Acc (type %d): %f %f %f`. Device orientation itself comes from CoreMotion's `CMDeviceOrientationManager`
  (`-[BKOrientationManager _queue_postUpdatedRawAccelerometerDeviceOrientation:]`), which on an iPhone gets it from
  locationd. lisa's root has no locationd binary (`usr/libexec/locationd` is in the 23F84 FileSystem dmg but left out
  of the lean root), hence the CoreMotion hook.
- Proximity: `-[BKHIDProximityEventProcessor processEvent:sender:dispatcher:]` reads field 0xe0000 (detection
  mask) and counts the object near when `mask & 0x440` (or `& 0x400`, by a flag). The AABC brightness code also
  checks for a service conforming to (0xff00, 0x08).
- Ambient light, CoreBrightness (loaded by backboardd): `IOHIDServiceClientConformsTo(service, 0xff00, 0x04)`
  (`CBABModuleiOS`, `AABC::registerService`, `CBDisplayContaineriOS`) or `(0x20, 0x41)` (`CBALSNode`,
  `CBDisplayContaineriOS matchDisplayWithHidService:`). Event type 12; `+[CBALSEvent replaceLuxWithFilteredLux:]`
  writes field 0xc000b (illuminance, double) and 0xc0000 (level, integer lux); `-initWithHIDEvent:andService:`
  reads 0xc000b first and falls back to 0xc0000; 0xc000a is the colour temperature, 0xc0006..9 the colour sample.
  `AABC::registerService` reads the service's `Built-In`, `DeviceUsagePairs`, `Orientation`, `Placement`,
  `ALSSensorType`, `FDRCalibration`, integration times, `ALSSensorMin/Max`, report intervals, and stops the ALS when
  its "initial factor" (a calibration value at AABC+0x118) is 0. The factor's source is not known yet: real auto
  brightness may need more than the events (the run below says).
- IOKit 23F84 constructors (argument registers read from the cache): `IOHIDEventCreateAccelerometerEvent`,
  `...GyroEvent`, `...CompassEvent` (alloc, timestamp, double x, y, z, options), `...AmbientLightSensorEvent` (alloc,
  timestamp, uint32 level, options), `IOHIDEventCreateProximtyEvent` (sic; alloc, timestamp, mask, options).

## The services (userland/motion/sensors-hid.c)

| Product | Primary page, usage | Also conforms to | Event |
| --- | --- | --- | --- |
| darwin-lisa accelerometer | 0xff00, 0x03 | | type 13, g, gravity as it pulls (upright: y = -1) |
| darwin-lisa gyroscope | 0xff00, 0x09 | | type 20, rad/s |
| darwin-lisa compass | 0xff00, 0x0a | | type 21, uT |
| darwin-lisa ambient light | 0xff00, 0x04 | 0x20, 0x41 | type 12, level = whole lux, 0xc000b = lux |
| darwin-lisa proximity | 0xff00, 0x08 | | type 14, mask 0x440 near / 0 far, sent on change only |

All built-in, `Transport` SPU. A service is made again until the event server opens it (20 tries, 2 s each, as the
touch screen). The thread reads `kern.hakios.sensors` every 20 ms and sends a sensor's event when the kernel's
count for it moves. Properties the server sets (`ReportInterval`...) are kept and logged; properties asked for and
missing are logged once each. Boot-arg `sensors_hid=0` leaves the services out.

Unit and axis conversions are in `userland/motion/sensor-events.c`, host-tested by `userland/motion/test/run-tests.sh`.

## Stage 2 (not built): locationd, for apps' CoreMotion

Apps' CoreMotion (CMMotionManager: raw accelerometer, gyro, compass, device motion) goes through locationd over XPC.
`usr/libexec/locationd` (40,226,848 B, sha256 `65a6306b...`, from the 23F84 FileSystem dmg) is not in the root:
`userland-26/scripts/select-system-26.py` drops every `usr/libexec`/`usr/sbin` program over `BIG_PROGRAM` (10 MiB).
Its LaunchDaemon plist stays, so launchd advertises its Mach services with no executable ("Missing executable",
exit 78) and synchronous clients wait. The same holds for 35 other daemons (21 in the FileSystem dmg, the rest
cryptex Safari/WebKit daemons; list in the scratch `missing-exec.txt` of this round).

What locationd needs, read from the binary: 164 libraries, 163 of them in the 23F84 dyld cache, the other
(`/usr/appleinternal/lib/liblinkguard.dylib`) weak. So it would load. Its motion side is `CLMotionCoprocessor` and
`SPUHIDDriverInterface` (the AppleSPU user client and its HID services, usage page 0xff0c), plus IOHIDEventSystemClient
matching; whether it falls back to plain (0xff00, 3) accelerometer services without an SPU is not known.

Proposal: a named exception, the limit unchanged: `BIG_PROGRAMS_KEPT = ('usr/sbin/bluetoothd', 'usr/libexec/locationd')`,
and a test root of its own (sensors2 + locationd, with `dlisa_oslog=locationd`). Risks: the CoreLocation migrator
hung DataMigrator for 20 min when locationd was in the root (`mkimage-trim.sh`, LEAN), and without an SPU locationd
may serve no motion at all. Pass: locationd stays up, no DataMigrator stall, and a CMMotionManager client gets
accelerometer data (`oslog[locationd]` says which service it took).

## Delivered (stage 1: accelerometer + rotation, and the services)

| Piece | What | sha256 |
| --- | --- | --- |
| kernel `research/hackios-xnu-kernel/kernel-16k-sensors1.bak` | `kernel-16k-s14-idle.bak` + `kernel/drivers/adsp/sensors-wire.py` (FastRPC file server, the current `dlisa.ssc.c`, 317 sensor files built in). Inert without `sensors=1` / `sensors_rpc=1` | `090fdadfd7367c73f5656c80e26356f342a483161121910ff377170219aa68ab` |
| `libdlisa-motion.dylib` | dlisa-motion.c + sensors-hid.c | `d948f0d57214b5c9bfa39f72705561a479d7b0e27c12a041159e10af0fa67221` |
| root `userland-26/out/system-26.apfs.full-sensors2` | the combined test root `system-26.apfs.full-8739d3b5` (int1 + libdlisa 174255e0 + HakiosMetal glass11 9e29ff9c + launchd dbeb1522 + InterfaceNamer aa6f1292) with the dylib above and `/etc/dlisa-inject.conf` e0704957 (= 8739d3b5's dd9abccb + `/usr/local/lib/libdlisa-motion.dylib backboardd`, the repository's conf since 5f8a74b). How: a file-by-file content diff of 8739d3b5 against `out/sysroot-26` (only those five files differ), a hard-link copy of the sysroot with the five taken from 8739d3b5 and the two of this round, `mkapfs.py -z` with `extract/system.manifest`, padded to 32 MiB. Read back: the seven files, and the whole image against its source copy | `57d7baec43027d2faa7599b7378fb797c9a3f2e1dfb6df43683d42aabefa562d` (4,999,610,368 B; 61212 files) |

To carry the change onto another root, those two files are all there is (the conf line must survive
`build-inject.sh`, which rewrites the conf: run `kernel/drivers/adsp/install-root.sh SYSROOT` after it).

Build notes: `.work/run-rebuild.sh` (apply.sh) is not idempotent once `sched-idle-quiet` sits on top of
`sched-prefer-select` (the reverse dry run of the latter fails and 4 of its 13 hunks go in again). This kernel was
built with steps 3 to 7 of `.work/rebuild26.sh` only, on the tree as the s14 build left it; `sched_prim.c` was put
back from the `.orig` patch kept. Builds on the shared tree go under
`flock research/hackios-xnu-kernel/.build.lock`.

## Test plan (autonomous: no one moves the phone)

Phone in pmOS first. Flash the root, then one host run. `E` has no GPU arguments; add the session's own if wanted.

```sh
cd ~/Dev/mobile
darwin-lisa/tools/flash-root.sh userland-26/out/system-26.apfs.full-sensors2      # reads back 57d7baec
E="dlisa_unlock=1 dlisa_noidle=1 display=1 console_open=1 lisa=1 keystore=1 init_run=/usr/libexec/usermanagerd:--init cpus=4 adsp=1 battmgr=1 sensors=1 sensors_rpc=1 sensors_fake=2 sensors_wait=150 sensors_rate=25"
KERNEL=kernel-16k-sensors1.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh sns1 "$E" "120 240 360"; bash darwin-lisa/tools/bootos.sh pmos
L=research/hackios-xnu-kernel/logs/sns1-end.log    # (gpu-quietrun's logs: sns1.log, sns1-tN.log, sns1-end.log)
grep -a -E '^fastrpc: ' $L | head -60
grep -a -E '^ssc: (the sensor core|service 400|.* enabled|sensors_fake)' $L
grep -a -E '^ssc: events:' $L | tail -5
grep -a -E 'dlisa-motion: (gravity|the device is now|hid: )' $L | head -40
```

Pass, in order (each step's failure names the next thing to fix):

1. FastRPC: `fastrpc: 317 sensor files (292 KB) to serve`, `stream 0x1805 goes through context N of the SMMU`,
   `ATTACHED TO THE SENSORS PD`, `THE SENSORS PD'S LISTENER IS UP`, then file calls (`fopen_with_env
   /vendor/etc/sensors/sns_reg_config -> 0`, `opendir .../registry -> 0`, the JSON).
2. SSC: `ssc: the sensor core has 1 sensor of type "accel"` (and gyro, mag, ambient_light, proximity as the registry
   configures them), `ssc: service 400 ...; SUIDs: accel gyro ...`.
3. Samples: `ssc: events: accel N` with N growing about 250 every 10 s at 25 Hz; the accel vector about 9800 mm/s2
   on the axis that points up (lying on the table: z). No `sensors_fake=2: ... made-up values` line.
4. iOS: `dlisa-motion: gravity ... (ADSP) -> face up` (or the orientation it rests in); every service
   `opened by the event server`; `hid: "darwin-lisa accelerometer" event 1..3` then every 1024; `ambient light`
   events when the light changes (room light: some lux > 0 in `ssc: events:`). The `asked for "..."` lines are what
   CoreBrightness/backboardd want and the services lack: copy them into the next round.
5. Rotation in the UI needs the phone turned; this autonomous run proves the data path. The rotation path itself was
   proven with `sensors_fake=1` (evening-b1b). For a rotation shot without a hand: run B.

Run B (iOS side alone, deterministic): the same `E` with `sensors=0 sensors_rpc=0 sensors_fake=1` (gravity turning a
quarter every 15 s, no DSP): `dlisa-motion: the device is now ...` every 15 s and the accelerometer service's events.
Light and proximity have no fake values.

Auto-brightness: with real lux, CoreBrightness either takes the service (no `Stopping ALS` in an `oslog[backboardd]`
run, `dlisa_oslog=backboardd`) or says why. That is data for the next round, not a pass criterion here.

Failure table: `kernel/drivers/adsp/README.md` (Boot 5 table in `docs/evening-session.md`): `DID NOT TAKE` ->
`sensors_rpc_sid=3` then 4; attach `REFUSED` 30 times -> its result and the DSP's last words; file calls `-> 2` for
a name that should exist -> add it to `fw-sensors-stage.py`; SSC lists only virtual sensors -> every `fastrpc:` line.

## Rollback

- Kernel: any earlier `KERNEL=` (`kernel-16k-s14-idle.bak`); nothing is flashed for the kernel.
- Root: flash the root that was on the phone before (`tools/flash-root.sh userland-26/out/system-26.apfs.full-<sha>`).
  Without reflashing: `sensors_hid=0` drops the HID services; without `sensors=1` and `sensors_fake` the hook gets
  no values and the screen stays as it is today.
- The xnu-work tree keeps the sensors wire (inert without boot-args); `sensors-wire.py` is idempotent.
