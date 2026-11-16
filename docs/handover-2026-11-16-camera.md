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
  runs the wire. In the 15:08 build `bsd/DEVELOPMENT/hakios.camera.o` compiled with no error; the kernel did not link
  because `osfmk/kern/sched_prim.c` was half-patched by the scheduler work (not ours). No camera kernel exists yet.

## Next

1. When the tree is in a known state: `sh kernel/drivers/camera/camera-build.sh kernel-16k-cam1` (wire + build under
   `research/hackios-xnu-kernel/.build.lock`, refuses to copy a kernel without the driver) ->
   `research/hackios-xnu-kernel/kernel-16k-cam1.bak`, sha256 at the end of `logs/kernel-16k-cam1-build.log`.
2. Check the other drivers stay inert without their boot-args (the camera adds no hook outside `hakios_camera_init()`).
3. The phone run of `docs/camera.md` section 6.
4. Then step B of the plan (`cam=5`, `/dev/hakios-camera`), after fixing HakiOS's read-path issues CAM-1/CAM-2
   (`research/hackios-xnu-kernel/docs/driver-review/camera-haptic-torch.md`).
