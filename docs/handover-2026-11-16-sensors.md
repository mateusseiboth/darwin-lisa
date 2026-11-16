# Handover 2026-11-16: sensors (SSC on the ADSP to iOS)

Everything is in `docs/sensors.md`; this is the short version.

## Where it is

- On the phone (evening-b1b, 11-10): ADSP up, SSC (QMI 400) answers, but no physical sensor: the sensors PD needs its
  registry and JSON over FastRPC, and the FastRPC file server has never run on the phone.
- Built now, not run: `research/hackios-xnu-kernel/kernel-16k-sensors1.bak` (`090fdadf...`) = s14-idle + the FastRPC
  server and the current SSC client; root `userland-26/out/system-26.apfs.full-sensors2` (`57d7baec...`) = the combined root 8739d3b5 + the new
  `libdlisa-motion.dylib` (`d948f0d5...`: rotation hook + five built-in HID sensor services in backboardd) + the inject
  line for it, which had been lost from every root since final1.
- The iOS side was read from the 23F84 backboardd and dyld cache: accelerometer (0xff00, 3) for BKOrientationManager
  and BKAccelerometerInterface, ambient light (0xff00, 4) or (0x20, 0x41) for CoreBrightness with lux in 0xc000b,
  proximity mask 0x440 in 0xe0000 for BKHIDProximityEventProcessor.

## Next

1. Run `sns1` (docs/sensors.md, "Test plan"). It says whether FastRPC attaches, whether the SSC lists the physical
   sensors, and whether real samples reach the HID services.
2. If the accelerometer streams: a rotation run with a hand, or keep `sensors_fake=1` for the screenshots.
3. From the `asked for "..."` lines: the ALS properties CoreBrightness wants (Placement, ALSSensorType, calibration),
   and why AABC may stop the ALS ("Initial factor =0"). Auto-brightness depends on that.
4. Apps' CoreMotion needs locationd, cut by the selector's 10 MiB rule: stage 2 in docs/sensors.md (named
   exception, own test root, DataMigrator risk).

## Watch out

- `.work/run-rebuild.sh` / `kernel/patches/apply.sh` reapply part of `sched-prefer-select` on a tree that has
  `sched-idle-quiet` (4 of 13 hunks). This round restored `osfmk/kern/sched_prim.c` from the `.orig` the patch kept,
  and built with steps 3 to 7 only. Shared-tree builds go under `flock research/hackios-xnu-kernel/.build.lock`.
- Fixed in 5f8a74b: `userland/inject/build-inject.sh` now installs libdlisa-motion and the repository's conf keeps
  its line (before, it rewrote the conf and dropped it).
