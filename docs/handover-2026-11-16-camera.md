# Handover 2026-11-16: cameras (host only)

Nothing was run on the phone and nothing was written to it.

## Where it stands

- Survey, iOS 26 findings and the plan with costs: [`docs/camera.md`](camera.md).
- Kernel driver: [`kernel/drivers/camera/`](../kernel/drivers/camera) (README: boot-args, log markers). HakiOS's redwood
  camera driver (same SoC, Camera.app works there) with lisa's four sensors; `cam=4` captures the macro S5K5E9's RAW10
  frames through CSIPHY 2, CSID-lite 0 RDI 0 and IFE-lite 0 into memory, and proves them in the log (levels per frame,
  auto-exposure, `camsnap` thumbnail, `camraw` crop) and on the data volume (`cam_dump=1`).
- Host tests: `sh kernel/drivers/camera/test/run-tests.sh` (C test of `camera_snap.h`, the S5K5E9 table against pmOS's
  Linux driver, every number against pmOS's DT and Linux 7.2.9's camcc/camss, the log-to-PNG tool): all pass.
  `tools/ksyntax.py` with the 26 kernel's flags: 0 errors.
- Wired into the shared xnu-work (`camera-wire.py`, option `HAKIOS_CAMERA`, inert without `cam=`), and `kernel/patches/apply.sh`
  runs the wire. The 15:08 build did not link (the scheduler's `sched_prim.c` was half-patched then); after the
  scheduler worker restored the tree, `sched_prim.o` was recompiled from its own compile record (the libpthread step
  needs it) and `camera-build.sh` made **`research/hackios-xnu-kernel/kernel-16k-cam1.bak`, sha256
  `3f32d6e949a4095d95bf92a3232feedfe3768b0d83cb3d4973b7dc3fcc226753`** (15:25; `llvm-nm` shows the camera's symbols).

## Next

1. The phone run of `docs/camera.md` section 6 with `KERNEL=kernel-16k-cam1.bak`.
2. Then step B of the plan (`cam=5`, `/dev/hakios-camera`), after fixing HakiOS's read-path issues CAM-1/CAM-2
   (`research/hackios-xnu-kernel/docs/driver-review/camera-haptic-torch.md`).
