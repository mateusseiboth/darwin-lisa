# Cameras on lisa (iOS 26): survey, kernel path, and the way into iOS

Host work only (2026-11-16): nothing was run on the phone. The kernel driver is `kernel/drivers/camera/` (its README
has the boot-args and log markers); the test plan is section 6.

## 1. The hardware

Source: pmOS's lisa device tree (`chroot_rootfs_xiaomi-lisa/boot/sm7325-xiaomi-lisa.dtb`), its sensor drivers
(`xiaomi-lisa-mainline/pmaports/device/community/linux-postmarketos-qcom-sc7280/0006..0013-*.patch`), Linux 7.2.9
(`cache_distfiles/linux-v7.2.9-sc7280.tar.gz`: camss, `camcc-sc7280.c`, `imx355.c`) and `xiaomi-lisa-mainline/docs/hardware.md`.
`kernel/drivers/camera/test/test_offsets.py` reads the same sources and checks the driver's numbers against them.

| | Macro | Wide (main) | Front | Ultra-wide |
| --- | --- | --- | --- | --- |
| Sensor | Samsung S5K5E9YX04, 5 MP | Samsung S5KGW3, 64 MP | Samsung S5K3T2, 20 MP | Sony IMX355, 8 MP |
| Linux driver | pmOS patch 0006 (out of tree) | patch 0010, new | patch 0012, new | mainline `imx355.c` |
| CCI, master | CCI1 (`ac4b000`) i2c-bus@0 | CCI0 (`ac4a000`) i2c-bus@1 | CCI0 i2c-bus@0 | CCI1 i2c-bus@1 |
| CCI pins (SDA/SCL) | GPIO 73/74 | GPIO 71/72 | GPIO 69/70 | GPIO 75/76 |
| I2C address | 0x2d | 0x10 | 0x10 | 0x1a |
| Chip ID | 0x559b at 0x0000 | | | 0x0355 at 0x0016 |
| Reset | GPIO 77 (high = running) | GPIO 21 | GPIO 20 | GPIO 78 |
| MCLK | camcc MCLK2, GPIO 66, 19.2 MHz | MCLK1, GPIO 65 | MCLK0, GPIO 64 | MCLK3, GPIO 67 |
| VANA | PM8008 L4P (2.7-2.9 V) | L6P 2.8 V | fixed 2.8 V, GPIO 106 | L7P (2.7-3.0 V) |
| VDIG | PM8008 L2P (0.95-1.2 V) | L1P 1.056 V | fixed 1.05 V, GPIO 108 | L2P |
| VIO | PM8008 L5P (1.7-1.9 V) | L5P | L5P | L5P |
| CSI-2 | 2 D-PHY lanes, CSIPHY 2 | 4 lanes, CSIPHY 1 | 4 lanes, CSIPHY 0 | 4 lanes, CSIPHY 3 |
| Link frequency | 480 MHz | 835.6 / 614.4 MHz | 310.5 MHz | 360 MHz |
| pmOS mode | 2592x1944 RAW10 GRBG, 30 fps | 4640x3472 / 4640x2612 (binned) GBRG, 30 fps | 2592x1940 (binned) GBRG, 30 fps | 1636x1232 RGGB, 30 fps |
| Focus | fixed | Dongwoon DW9800 (no Linux driver) | fixed | fixed |

The PM8008 (`qcom,pm8008`) is on QUP SE1's I2C (GPIO 4/5, 100 kHz), addresses 0x08 (chip) and 0x09 (regulators); its
reset is PM8350C GPIO 3. All supplies `regulator-fixed` switch from `vreg_bob` / `vreg_s8b`.

The SoC side (`qcom,sc7280-camss`, Titan 165), addresses:

| Block | Address | Clocks (camcc) |
| --- | --- | --- |
| CSIPHY 0..4 | `0xace0000 + 0x2000 n` | `csiphyN` (branch), `csiNphytimer` (RCG 300 MHz) |
| CSID 0, 1, 2 (full) | `0xacb3000`, `0xacba000`, `0xacc1000` | `vfeN_csid` |
| CSID-lite 0, 1 | `0xacc8000`, `0xaccf000` | `vfe_lite0_csid` (RCG `0xc020`, 400 MHz) |
| IFE (VFE) 0, 1, 2 | `0xacaf000`, `0xacb6000`, `0xacbd000` | `vfeN`, `vfeN_axi` |
| IFE-lite 0, 1 | `0xacc4000`, `0xaccb000` | `vfe_lite0` (RCG `0xc004`, 400 MHz), `vfe_lite0_cphy_rx` |
| CCI 0, 1 | `0xac4a000`, `0xac4b000` | `cci0`, `cci1` |
| camcc | `0xad00000` | PLL0 (Lucid, 1200 MHz), titan top GDSC `0xc194` |
| apps SMMU stream | `0x800`, mask `0x4e0` | |

Format: every sensor sends MIPI CSI-2 RAW10 (data type 0x2b), written to memory as packed RAW10 (four pixels in five
bytes) by the IFE's RDI write master. There is no ISP on this path: what comes out is the Bayer mosaic.

## 2. What Linux and pmOS have, and what they lack

- Mainline `qcom-camss` on sc7280: CSIPHY (3PH 1.0 driver, D-PHY lane table `lane_regs_sm8250`, C-PHY for sc7280 not
  correct upstream: `research/fp5-imx800/camss/`), CSID gen2 (full and lite), VFE 170 **RDI only**. The IFE's pixel
  path (demosaic, statistics, scaler) is not supported in mainline: Qualcomm's ISP is proprietary (CamX/Chromatix).
  CCI: `i2c-qcom-cci.c`.
- pmOS runs all four cameras through libcamera's **soft ISP** (debayer, simple AWB/AGC on the CPU/GPU), RDI raw frames
  only.
- Lacking in pmOS: libcamera sensor helpers for S5K5E9, S5KGW3, S5K3T2 (gain control rough); the DW9800 lens motor (no
  driver: the wide camera has no focus); IMX355 full resolution (3272x2464) resets the phone; Snapshot sometimes fails
  to start; no hardware ISP at all.

## 3. The kernel path on XNU (done on the host)

HakiOS (`research/hackios-xnu-kernel/drivers/camera/hakios.camera.c`) already drives redwood's cameras up to
Camera.app on iOS 16, on the **same SoC**. Everything of the SoC is identical on lisa: camcc and its RCG/branch
offsets, the GDSC, CSIPHY 0..4, CSID-lite 0, IFE-lite 0 RDI, the SMMU streams, RPMh's CX/MX, the PM8008 (same bus,
same reset pin), even the CCI, MCLK and reset pins of each position. Only the sensors differ. So the lisa driver is
HakiOS's driver with lisa's sensor table (`kernel/drivers/camera/`):

- sensor: the macro **S5K5E9**, the simplest: 2 lanes, 2592x1944 RAW10, fixed focus, every supply a PM8008 LDO, a
  register table of about a hundred writes;
- I2C by hand on the CCI's pins (HakiOS's way), PM8008 LDOs on its bit-banged I2C;
- CSIPHY 2 in D-PHY mode (the write sequence equals Linux's `lane_regs_sm8250[]`), settle count 21 (Linux's formula at
  480 MHz with the 300 MHz timer);
- CSID-lite 0 RX from CSIPHY 2 into RDI 0, IFE-lite 0 write master 0 writing each frame to memory through a context
  of the apps SMMU (stream 0x800);
- the sensor's tables made by `camtab.py` from pmOS's own Linux driver (the CamX module files of lisa's vendor image
  are not on this machine), built into the kernel;
- proof of the path without the loader handing memory back (the 16 KB kernel does not return to it): per-frame levels
  in the log, an RGB thumbnail (`camsnap`), an optional full-resolution crop (`camraw`), and the frames written to the
  data volume (`cam_dump=1`). A plain auto-exposure between frames.

Built: section 6.

## 4. iOS 26: what the capture stack expects

From the 23F84 dyld cache (iPad17,1 IPSW) and the root:

- The camera server is `cameracaptured` (`/usr/libexec/cameracaptured`, launchd `com.apple.cameracaptured`): it starts
  `FigCaptureSourceServer`, `FigCaptureSessionServer`, `FigFlashlightServer`, all in `CMCapture.framework`. mediaserverd
  is out of the camera path since iOS 17.
- Devices come from `BWFigCaptureDeviceVendor`, still built as in iOS 16:
  `-[BWFigCaptureDeviceVendor initWithDefaultDeviceCreateFunction:cmioExtensionDeviceCreateFunction:cmioDiscoverySession:]`.
  The default create function is found by `fcc_loadCaptureDeviceCreateFunction` / `FigCaptureGetCaptureDeviceCreateFunction`,
  which loads `/System/Library/MediaCapture/H{10,13,16}ISP.mediacapture` and takes its `HnnISPCaptureDeviceCreate`.
  This is the seam HakiOS used on iOS 16 (its `camera_device.c` gives the vendor its own create function, inside
  mediaserverd).
- The H16 ISP device talks to the kernel's `AppleH16CamIn` (IOKit user client) and firmware run by `appleh16camerad`,
  with `ISPExclaveKitServices` on newer chips. Imitating `AppleH16CamIn` would mean imitating Apple's ISP firmware
  protocol: not realistic.
- There is a second, newer seam: `cmioExtensionDeviceCreateFunction` with a CMIO discovery session
  (`_handleCMIOExtensionDevicesChangedNotification`): CoreMediaIO camera extensions, the way iPadOS 17+ serves USB
  (UVC) cameras (`isUsingIPadExternalCamera`, `kTCCServiceExternalCameraMedia`). It is iPad-only and gated.
- Per-model configuration: `CMCapture.framework/<model>/AVCaptureSession.plist` (+ `CameraSetup.plist`,
  `*-ModuleConfig.plist`, `*-IQTuning.plist`). This IPSW has only J817, J818, J820, J821 (the iPad mini's). The port
  presents an iPhone 13 mini (D16), which has none here.

## 5. The way into iOS: plan and honest costs

Not viable: a device that Apple's own pipeline accepts as an ISP camera (`AppleH16CamIn` + `appleh16camerad` +
H16ISP.mediacapture). That needs Apple's ISP firmware interface, its frame metadata and its exclaves.

Realistic: HakiOS's route, moved from mediaserverd to cameracaptured. Steps:

| # | Step | What | Cost | Risk |
| --- | --- | --- | --- | --- |
| A | Sensor frames in the kernel | `cam=4` (done on the host; test below) | done, 1 phone run | PM8008 L2P at 1.2 V, sensor answer, SMMU |
| B | `/dev/hakios-camera` (`cam=5`) on lisa | HakiOS's code is in the driver already (ring of 3, reads, exposure writes, sensor switch); test with HakiOS's `camera_test.c` built for iOS 26 | 1-2 days | the read path's known HakiOS issues (driver review CAM-1, CAM-2) to fix first |
| C | Other sensors | `camtab.py` for IMX355 (mainline tables) and the S5KGW3/S5K3T2 patches (CamX-derived tables, 4 lanes) | 1 day each with a phone run | front's two GPIO supplies (done), GW3's 835 MHz link, IMX355 full-res reset seen on pmOS |
| D | A FigCaptureDevice in cameracaptured | port HakiOS `camera_device.c` + `camera_isp.c` (CPU debayer, AWB, gamma, scaling; libhakios.media) into libdlisa, injected into cameracaptured (`/etc/dlisa-inject.conf`), handing `BWFigCaptureDeviceVendor` our create function in place of `H16ISPCaptureDeviceCreate`. The CMBaseObject class tables and stream/device property keys must be re-read from 26's CMCapture (16.5's layout will have moved) | 1-2 weeks | the largest: 26's private ABI, Swift parts of CMCapture, sandbox/TCC of cameracaptured, the vendor's power assertion and "camera stolen" logic |
| E | Model plist | a `CMCapture.framework/D16/AVCaptureSession.plist` (or present the J817 identity to cameracaptured), with formats named as the plist expects (2592x1944 / 1920x1080 / 1280x720 at 30) | 2-3 days | AVFoundation format matching; MobileGestalt camera capabilities |
| F | Debayer on the GPU | the CPU debayer of 5 MP at 30 fps is about 150 M pixel operations/s on an A78; a Metal (or GL via our Adreno path) kernel later | 1 week | depends on the GPU work |

Alternative to D-E: a CMIO camera extension (the iPad's external-camera path), which Apple's stack already treats as a
non-ISP camera with plain pixel buffers. Cheaper on the ABI side (public CMIOExtension API) but iPad-only and gated by
`isUsingIPadExternalCamera`/TCC; worth a probe once D's injection exists, since it would avoid the private device ABI.

What is implemented now: A (kernel capture, the frames proven in the log and on the data volume). B exists in the
driver as HakiOS wrote it and is untried on lisa.

## 6. Built kernel and test plan

| Piece | What | sha256 |
| --- | --- | --- |
| `research/hackios-xnu-kernel/kernel-16k-cam1.bak` | the tree as of 2026-11-16 15:05 (the last build before this one made `kernel26.nopac`, with the other workers' wires) + `HAKIOS_CAMERA` | see `kernel/drivers/camera/README.md` / the handover |

Inert without `cam=`: the driver starts nothing, maps nothing. The camera is not in `build-combined.sh`'s lines.

### Test (one host run, nobody needed at the phone)

The phone parked in pmOS. The data volume (`var_ufs=1 ...` as the current host runs have it) for the dump; without it
the log still carries the proof.

```sh
cd ~/Dev/mobile
E="<the usual host-run line> cam=4 cam_after=60 cam_frames=24 cam_snap=64 cam_crop=256 cam_dump=1"
KERNEL=kernel-16k-cam1.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh cam1 "$E" "120 180 240"; bash darwin-lisa/tools/bootos.sh pmos
python3 -I darwin-lisa/kernel/drivers/camera/camsnap.py logs/cam1-end.log logs/cam1-snap.png --scale 4
```

`cam_after=60`: the capture after the boot's busiest minute, its ~150 KB of log lines inside the run's log.

Pass, in this order (each line names where it stops otherwise):

1. `camera: cam=4: the macro (S5K5E9) camera`, `the macro (S5K5E9) sensor's tables: slave 0x5a, chip ID 0x559b`
2. `the PM8008i answers`, `PM8008i LDO5 on at 1800 mV`, `LDO4 on at 2800 mV`, `LDO2 on at 1200 mV`, `MCLK on at 19.2 MHz`
3. `the macro (S5K5E9) sensor's chip ID: 0x559b (as Android expects)` -> the sensor is powered and on the bus
4. `the sensor's "init": 42 writes`, `"res0": 57 writes`, `CSIPHY 2 configured: 2 lane(s), D-PHY (lanes enabled 0x85), settle count 21`
5. `camera: frame 1 written, N ms after the sensor was started` -> **the CSI-2 path works**
6. `camera: frame N: R .. G ../.. B ..`: levels that are not all 0 or all 1023, checksums that differ frame to frame;
   `auto-exposure: ...` lines bringing G toward ~256
7. `camsnap 24 64x48 ...` (96 lines) -> `logs/cam1-snap.png` shows the scene (rotated 270 degrees, as mounted);
   `logs/cam1-snap-crop-24.png` and `.raw10`: the centre at full resolution
8. `camera: cam_dump: /private/var/camera-S5K5E9.raw: N bytes written`; on pmOS later, the data volume's file (read
   it with the volume copied to the PC: `docs/data-volume.md` A8, `mount -t hfsplus -o ro,loop`), then
   `research/hackios-xnu-kernel/tools/camraw.py camera-S5K5E9.raw logs/cam1-raw`
9. `camera: done`; nothing after it from the camera; the run goes on as without `cam=` (SpringBoard up).

If it stops: at 2 (LDO not ready): `cam=1` shows the PM8008 state; at 3 (no answer): bus pins/reset (the `gpio73/74/77`
lines of `cam=1`), try `cam_sensor=ultra` with `cam=2` for another sensor's ID once its table exists; at 5 (no frame):
`show_paths` and `CSIPHY 2 ... interrupt status` lines, try `cam_settle=` 15..30 and `cam=3` (the CSID's colour bars:
no sensor, proves CSID/IFE/SMMU alone); SMMU faults are printed by `smmu_faults()`.

Rollback: boot without `cam=` (the driver does nothing), or the previous kernel (`KERNEL=kernel-16k-s14-idle.bak`).
Nothing is written to the flash except, with `cam_dump=1`, one file on the data volume.
