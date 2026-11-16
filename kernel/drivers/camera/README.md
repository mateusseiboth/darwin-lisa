# Cameras (lisa, iOS 26)

HakiOS's camera driver for the POCO X5 Pro (redwood, the same SM7325) with lisa's four sensors in place of redwood's.
The SoC side (camcc, titan top GDSC, CSIPHY, CSID-lite 0, IFE-lite 0 in RDI mode, the apps SMMU, the PM8008 that feeds
the sensors) is the same on both phones and stays as HakiOS wrote it. On redwood it runs Camera.app (`cam=5`). The
survey behind it, and the plan for iOS: [`docs/camera.md`](../../../docs/camera.md).

What runs today is `cam=4`: one sensor, the macro S5K5E9, powered up, its frames captured RAW10 through CSIPHY 2,
CSID-lite 0 RDI 0 and IFE-lite 0 into memory, and logged, with no ISP.

## Files

| File | What |
| --- | --- |
| `hakios.camera.c` | the driver (bsd/dev). The changes from HakiOS: lisa's `sensors[]`, built-in tables first (`builtin_tab()`), two GPIO supplies per sensor (the front camera's VANA and VDIG), `exposure()` for any SMIA sensor, the per-frame log, the auto-exposure, the `camsnap` thumbnail and `frames_dump()`, the lens motor opt-in (`cam_af=1`), `cam=5` on the macro |
| `hakios.camera.h` | `/dev/hakios-camera`'s interface (`cam=5`), HakiOS's, unchanged |
| `camera_snap.h` | the frame statistics, the thumbnail rows and the exposure step: pure C, host-tested |
| `camtab.py` | the sensor tables (HakiOS's `RWCTAB02`) made from pmOS's Linux sensor drivers, as a C header |
| `camsnap.py` | the log's `camsnap` lines back into a PNG |
| `camera-wire.py` | tag `hakios_camera` (option `HAKIOS_CAMERA`) in the xnu-work tree; `kernel/patches/apply.sh` runs it too |
| `camera-build.sh` | wire + rebuild under `research/hackios-xnu-kernel/.build.lock`, into `research/hackios-xnu-kernel/<NAME>.bak` |
| `test/` | `run-tests.sh`: the C test of `camera_snap.h`, `test_camtab.py`, `test_offsets.py`, `test_camsnap.py` |

The tables are GPL-2.0 data of the Linux drivers (`xiaomi-lisa-mainline/pmaports/.../0006-media-i2c-Add-S5K5E9-camera-sensor-driver.patch`):
`camtab.py` reads them at wire time and they never enter git.

## What is checked on the host

- `test_camtab.py`: the S5K5E9 table replays `s5k5e9_power_on()`, `s5k5e9_start_stream()` (common list, mode list,
  `__v4l2_ctrl_handler_setup()` writes in group hold, `0x0100 = 1`) and `s5k5e9_power_off()` write for write; the
  settle count is Linux's `csiphy_settle_cnt_calc()` (21 at 480 MHz); slave, chip ID, lanes as the device tree and probe.
- `test_offsets.py`: every sensor's CCI pins, reset GPIO, MCLK RCG/branch/pin, CSIPHY and its clocks, supplies (PM8008
  LDO number and a voltage inside the device tree's range, or the fixed regulator's GPIO), the PM8008's bus and reset
  pin, the CAMSS block addresses and SMMU stream, the titan clocks against Linux 7.2.9's `camcc-sc7280.c`, and the
  CSIPHY D-PHY lane sequence against Linux's `lane_regs_sm8250[]` (what sc7280 uses), write for write.
- `camera_snap_test.c`: RAW10 unpacking, statistics, thumbnail, exposure steps.
- `tools/ksyntax.py` with the 26 kernel's flags: 0 errors (the warnings are HakiOS's kinds: cast-align, unsafe-buffer).

Not checkable here: whether the PM8008 LDOs come up (L2P at 1.2 V is the top of its range), whether the S5K5E9 answers at
0x2d, whether frames arrive, the SMMU. The log says each.

## Boot-args

| Boot-arg | What |
| --- | --- |
| `cam=1` | a look, read only: clocks, pins, the PM8008 and the sensor's LDOs |
| `cam=2` | the sensor powered up, its chip ID read (0x559b), powered down |
| `cam=3` | the CSID's colour bars through RDI 0 into memory (no sensor) |
| `cam=4` | the sensor's frames into memory, logged |
| `cam=5` | `/dev/hakios-camera` for a user-space program (HakiOS's, untried on lisa) |
| `cam_sensor=macro\|wide\|front\|ultra` | which sensor (only the macro has its table) |
| `cam_frames=N` | how many frames (default 24, at most 300; the auto-exposure steps every third) |
| `cam_after=S` | seconds after boot before it starts (default 20) |
| `cam_ae=0` | no auto-exposure (it starts from Linux's defaults: the longest exposure at 32x gain) |
| `cam_shutter=L cam_gain=G cam_vblank=V` | a fixed exposure instead (S5K5E9: gain 32 = 1x, up to 1024) |
| `cam_snap=COLS` | the thumbnail's width (default 64, rows 3/4 of it; 0 for none) |
| `cam_crop=W` | a full-resolution centre crop of the last frame, W x 3W/4 (at most 512), as `camraw` lines: its RAW10 bytes |
| `cam_dump=1` | the frames to `/private/var/camera-S5K5E9.raw` once `/private/var` is the data volume (HFS, `var_ufs=1`; `cam_dump_wait=S`, default 240). `cam_dump=2`: whatever it is on |
| `cam_pattern=1`, `cam_settle=N`, `cam_mode=N`, `cam_keep_mb=MB`, `cam_af=1` | HakiOS's |

## Log markers (cam=4)

```
camera: cam=4: the macro (S5K5E9) camera: its frames through its CSIPHY and RDI 0 into memory
camera: the macro (S5K5E9) sensor's tables: slave 0x5a, chip ID 0x559b at 0, lanes 0x10, 1 modes, 6 tables
camera:   mode 0: 2592x1944 at 0,0, 2 lane(s), D-PHY, settle 21, pixel clock 192000000, line 3112, frame 2030, 30.393 frames/s
camera: PM8008i LDO5 on at 1800 mV (status 0x87), then LDO4 at 2800, LDO2 at 1200
camera: MCLK on at 19.2 MHz
camera: the macro (S5K5E9) sensor's chip ID: 0x559b (as Android expects)
camera: CSIPHY 2 configured: 2 lane(s), D-PHY (lanes enabled 0x85), settle count 21
camera: frame 1 written, N ms after the sensor was started
camera: frame 1: R .. G ../.. B .. (10-bit means of N cells), levels a..b, x.y% saturated, checksum ........
camera: auto-exposure: L lines, gain G for the frames after 1
camsnap 8 64x48 <row> <part> <hex>          (96 lines)
camera: cam_dump: /private/var/camera-S5K5E9.raw: N bytes written (0, 0 left)
```

`python3 -I kernel/drivers/camera/camsnap.py LOG snap.png --scale 4` makes the thumbnail a picture (and, with `cam_crop`,
`snap-crop-<frame>.raw10` and `.png`);
`research/hackios-xnu-kernel/tools/camraw.py camera-S5K5E9.raw out` debayers the dumped frames (half resolution).
