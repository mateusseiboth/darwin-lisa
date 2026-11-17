# Peripherals of lisa on iOS 26 (2026-11-16, host only)

What each remaining peripheral is, how pmOS / Android drive it, what iOS 26 (23F84) expects, what is done here and
what is left, with an honest cost. **Nothing here has run on the phone.** Every driver is inert without its boot-arg
(the keys excepted: on with `lisa=1`, read only).

Sources: pmOS's `sm7325-xiaomi-lisa.dtb` (decompiled); the stock lisa dtbo (`stock/extract/.../dtbo.img`, entry "lisa
based Qualcomm Technologies, Inc. SM7325"); Xiaomi's kernel (MiCode `lisa-r-oss`); Linux 7.2.9 (pmOS distfile);
HakiOS (redwood, iOS 16, `research/hackios-xnu-kernel`); static reading of the 23F84 binaries (bluetoothd, BlueTool,
the dyld cache, the reference kernelcache).

| Item | Hardware | Kernel | iOS side | Where |
| --- | --- | --- | --- | --- |
| Side / volume keys | PMK8350 PON (KPDPWR, RESIN), PM7325 GPIO6 | done | done (already in libdlisa) | `kernel/drivers/buttons/` |
| Vibration | **Awinic AW8624**, I2C 0x5a (GPIO 16/17), reset GPIO 35 | done | plan (needs a `vibrator` IOService) | `kernel/drivers/haptic/` |
| Flashlight | PM8350C flash LED module, channel 0 | done | done (libdlisa, SpringBoard) | `kernel/drivers/torch/`, `userland/inject/torch-level.c` |
| Panel brightness | VTDR6130 DCS 0x51 over DSI | not started | plan | below |
| Bluetooth | WCN6750 BT half, UART7 (QUP0 SE7), BT_EN GPIO 85 | done (bt=1..4 + bluetoothd bridge) | virtual controller done; real path planned | `kernel/drivers/bt/`, `userland/inject/bt-virtual.c` |
| Speaker | 2x TFA9873 (I2C SE3) on PRIMARY MI2S, ADSP APR | done (sound=1..4, Xiaomi's tuning, status guard) | built, not run (DLisaAudio.driver, [audio-ios.md](audio-ios.md)) | `kernel/drivers/audio/`, `userland/audio/` |
| NFC, fingerprint, modem | survey at the end | | | |

## Builds and files

| Piece | What | sha256 |
| --- | --- | --- |
| `research/hackios-xnu-kernel/kernel-16k-periph2.bak` | periph1 + vibration: under-voltage noted, not fatal + bt=2 votes l19b, s1c, l11c through RPMh | `d9b0fd3950048f18b4da26faa7a2eb76286139eeec1e9ff923e139d3dfd80ef9` |
| `research/hackios-xnu-kernel/kernel-16k-periph1.bak` | the shared tree as of 2026-11-16 15:2x (s14 + the restored scheduler `sched_prim.c` 3c766b2f + the other workers' wires) + buttons, torch, haptic, bt, audio; firmware built in (`aw8624_haptic.bin`, `msbtfw11.{mbn,tlv}`, `msnv11.bin`). Built by `kernel/drivers/periph-build.sh` (lock, no `apply.sh`, symbols checked) | `40815677f8d56468e31549d8c58cdbdbeae59f057de6cd24bd1f28d2dd5754e0` |
| `userland-26/out/system-26.apfs.full-9a91ed47` | test root: int3 (122a0d5c: libdlisa 7249bc09 with the Bluetooth and flashlight hooks, libdlisa-motion d948f0d5, ld-groups 2beca308, launchd dbeb1522, HakiosMetal 9e29ff9c, InterfaceNamer aa6f1292) + `usr/sbin/bluetoothd` (23F84, aaa02518) + `/etc/dlisa-inject.conf` of HEAD (b6d0f0b4: int3's + bluetoothd; both libraries it names are present). Hard-link copy of `out/sysroot-26` with those files replaced (taken out of the int3 image with `apfs.py cat`), `mkapfs.py -z` with `extract/system.manifest`, padded to 32 MiB; copy deleted, shared sysroot untouched. Read back: the files above; census 61213 files (int3 + 1), 24194 directories, 821 symlinks, 15960 checksums, 0 bad. 4,999,610,368 B. NOT written | `9a91ed478bbd3d7d6a6439f148192ea8e6b14b0116c028d83184fe74e817706f` |

How to rebuild: `sh kernel/drivers/periph-build.sh NAME` (`EXTRA="bt audio"` by default). It holds
`research/hackios-xnu-kernel/.build.lock`, runs only the idempotent wires of this work, never writes `kernel26.nopac`,
and copies nothing unless the linked kernel is newer than the run and defines every driver's entry symbol.

## 1. Side and volume keys

- **Hardware**: power = KPDPWR_N of the PMK8350 power-on block (SPMI slave 0, 0x1300), volume down = RESIN_N of the
  same block, volume up = GPIO6 of the PM7325 (slave 1, 0x8d00), pulled up. Same as redwood.
- **pmOS**: `pm8941-pwrkey` (pmk8350 pwrkey/resin) and `gpio-keys`.
- **iOS**: backboardd's HID event system; consumer page Power 0x30, Volume Increment 0xE9, Decrement 0xEA from a
  built-in service. libdlisa's "darwin-lisa buttons" virtual service already turned `/dev/hakios-touch` button records
  into exactly that (it also wakes the display on a tap). Lock, wake, volume HUD, screenshot (side + volume up), long
  press are backboardd/SpringBoard's own.
- **Done**: `dlisa.buttons.c`, HakiOS's redwood driver on the shared SPMI code. Reads only, every 16 ms; a key held at
  boot is not believed until released. Test hooks `buttons_seq=`, `kern.hakios.buttons_*`.
- **Left**: interrupts instead of polling (only matters once the system sleeps).

## 2. Vibration

- **Hardware**: lisa has an **AW8624** (ID 0x24), not redwood's AW86927 and not the PMIC's haptics: stock overlay
  `aw8624_haptic@5A` on `qupv3_se4_i2c`, reset GPIO 35, irq GPIO 18; waveforms `/vendor/firmware/aw8624_haptic.bin`
  (1..10 short effects, 11 looped). pmOS has no driver (a comment in its dts).
- **Done (kernel)**: `dlisa.haptic.c` (haptic=1..5): I2C by hand on GPIO 16/17, reset, ID, Android's probe
  configuration (less the f0 calibration), the RAM loaded and read back, RAM effects and the looped waveform, fault
  flags polled. `/dev/hakios-haptic` and `kern.hakios.haptic_play`: 0 stop, 1..10 an effect, 1000+ms a vibration.
- **iOS** (research, unverified on the phone): `FigVibrator` in audiomxd chooses. With MobileGestalt
  `DeviceSupportsClosedLoopHaptics` (DT `/product/haptics closed-loop`, which our identity sets to 1) it goes to
  VibeSynth / `AVHapticServer`, which on 26 waits for an `AHSFirmwareReadyNotifier` IOService and an "Actuator" audio
  route: the HakiOS HAL-plug-in way, behind the whole audio work. With `closed-loop` = 0 it uses
  `IOServiceGetMatchingService(IOServiceNameMatching("vibrator"))` and `IORegistryEntrySetCFProperties` with
  `{"hertz_millisecs": [{"intensity": f}, onMs, false, offMs...], "repeat": bool}` (`FVIOKit_StartIOServiceVibration`),
  which covers ringer, alerts, `AudioServicesPlaySystemSound(4095)` (not UIKit's taps).
- **Plan (about 1 day)**: an IOKit class registered under the name `vibrator` whose `setProperties()` parses that
  dictionary and calls the haptic driver's command function; at its start (haptic=5) it sets `closed-loop` = 0 on
  `IODeviceTree:/product/haptics` in the registry, so nothing in the loader changes; the MobileGestalt cache has to be
  removed once (`identity-stamp.c` only removes it when hw.machine/model change).

## 3. Flashlight

- **Hardware**: PM8350C flash LED module (slave 2, 0xee00), channel 0 for the rear LED (stock `switch_0`
  `qcom,led-mask` 0x01), 125 mA (Android's torch on redwood). Linux `leds-qcom-flash.c` register map.
- **Done (kernel)**: `dlisa.torch.c` (torch=1..3), whitelist of six registers, every write read back, writes only if
  EE 0 owns the arbiter channel, guards ten times a second, at most 120 s on.
- **iOS**: Control Center's `FlashlightModule` needs MobileGestalt `camera-flash` (our identity has it); the module
  talks to `AVFlashlight` (AVFCapture) in SpringBoard, which asks cameracaptured (no camera here).
- **Done (iOS)**: libdlisa in SpringBoard, when `kern.hakios.torch_status` says the driver serves Darwin, answers
  `AVFlashlight`'s `isAvailable`, `isOverheated`, `turnPowerOnWithError:`, `setFlashlightLevel:withError:`,
  `turnPowerOff`, `flashlightLevel`, `beamWidthControlSupported`, writes the level to `/dev/hakios-torch` and announces
  `flashlightLevel` by KVO. Not covered: Siri / Shortcuts from other processes.

## 4. Panel brightness (plan only)

- **iOS**: the slider ends in backboardd's CoreBrightness: `BrightnessSystem setProperty:forKey:` ("DisplayBrightness",
  0..1) -> `CBDisplayModuleiOS` -> an `AppleARMBacklight` service (`backlight-control` = true; property
  `brightness-nits`, 16.16) and/or `IOMFBBrightnessLevel` on the framebuffer. `MiniIOMobileFramebuffer.cpp` has
  nothing for brightness today.
- **Missing, and the real cost**: a DCS write from the kernel (MDSS DSI0 command DMA: `0x51` + two bytes, the
  VTDR6130 range per Linux `panel-visionox-vtdr6130`) while the panel runs in command mode with the firmware's setup:
  1 to 2 days with the phone, as it touches the live display path (display owner).
- **Then**: libdlisa in backboardd hooks `BrightnessSystem setProperty:forKey:` for `DisplayBrightness` /
  `DisplayBrightnessFactor` and writes a sysctl (half a day); an `AppleARMBacklight` stand-in only if auto-brightness
  is wanted.

## 5. Bluetooth

- **Hardware**: WCN6750's BT half on UART7 (QUP0 SE7, 0x99c000, GPIO 28-31 `qup07`), BT_EN GPIO 85, SW_CTRL 86,
  supplies shared with Wi-Fi plus `l19b`, `s1c`, `l11c`. Firmware: `qca/msbtfw11.mbn` + `msnv11.bin` (pmOS).
- **Linux**: `hci_qca` + `btqca`: QCA version (0xFC00), rampatch + NVM TLV download, baud change, HCI reset.
- **Done (kernel)**: `dlisa.bt.c` (bt=1 look, bt=2 UART + version + HCI reset in ROM, bt=3 full setup at 3 Mbit/s +
  LE scan + inquiry logged, bt=4 serving). bt=4 gives `/dev/hakios-bt` (raw H4) and the bridge bluetoothd wants:
  kern_ctl `com.apple.uart.bluetooth` (datagram, unprivileged), TIOCGETA/TIOCSETA answered, `/dev/uart.bluetooth`,
  Broadcom vendor commands answered locally, every HCI opcode logged. See `kernel/drivers/bt/README.md`.
- **iOS** (static reading of bluetoothd 23F84): transport from IORegistry `bluetooth`/`marconi-bt`
  `transport-encoding`, boot-arg `bt-transport-override=N`, env `BT_HCI_TRANSPORT`; the H4 "POSIX" driver opens the
  kern_ctl named in its parameter (`com.apple.uart.bluetooth`); main() needs `/dev/uart.bluetooth` or such a node;
  BlueTool boots chips with Broadcom/ACI scripts (unusable on Qualcomm); a built-in **virtual controller** (transport
  10, chipset APPLE_VIRTUAL 6000) is used when `kern.hv_vmm_present` is non-zero.
- **The root**: `usr/sbin/bluetoothd` (12.2 MB) was cut by `select-system-26.py`'s 10 MiB rule (that is why launchd
  said "Missing executable"); 3c73a31 keeps it by name. BlueTool, BTLEServer, bluetoothaudiod, bluetoothuserd were
  already there.
- **Done (UI step)**: libdlisa in bluetoothd (`bt-virtual.c`, boot-arg `dlisa_btvirt=1` with `bt-transport-override=10`)
  answers `kern.hv_vmm_present` = 1 to bluetoothd alone (its GOT entries for `sysctlbyname` rebound): it builds the
  virtual controller and never calls BlueTool. Expected: Bluetooth "On" in Settings and Control Center, no real radio.
- **Real path, next** (2 to 5 days after the kernel levels pass on the phone): bt=4; bluetoothd's plist environment
  `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000`; an IORegistry/DT `bluetooth` node with
  `transport-encoding` 1 (no `local-mac-address`); BlueTool replaced by a responder that answers `RunBuiltinScript`
  with `{result: 0, manufacturer: 29, firmware: "WCN6750"}`; then fill the vendor-command answers from the bridge's log
  until bluetoothd reports powered on.

## 6. Speaker

- **Hardware**: two NXP TFA9873 on I2C SE3 (0x34 top/earpiece, 0x35 bottom), I2S on PRIMARY MI2S (GPIO 97-100), fed
  by the ADSP. pmOS: q6asm MultiMedia1 -> q6routing -> q6afe PRIMARY_MI2S_RX, `tfa9872.c` for the amps, no DSP tuning.
- **Done (kernel)**: `dlisa.sound.c` + `dlisa.tfa9873.c`, sound=1..4 with adsp=1 (port + clocks; + SMMU/ADM/ASM and a
  silent -20 dBFS tone; + amps, audible at -32 dBFS; serve `/dev/hakios-audio`). Uses the hooks hakios.adsp.c already
  calls. See `kernel/drivers/audio/README.md` (safety limits and risks: the audio PD may need PDR/servreg to come up).
- **iOS**: audiomxd owns audio; HAL plug-ins load from `/Library/Audio/Plug-Ins/HAL` (not /System); HakiOS's
  `audio_plugin.c` (AudioServerPlugIn, `/dev/hakios-audio`) is the model. Unknowns: VirtualAudio's topology for this
  product and the missing speaker tunings ("HAL Speaker Protection is missing" may fail the route or attenuate 20 dB).
- **Cost to iOS sound**: the plug-in port is small (a day); VirtualAudio route work is open-ended (days).
- **2026-11-16 (host)**: the amplifiers take Xiaomi's tuning where it fits pmOS's bus (`sound_tune`, the current limits
  and the boost of the stock `tfa98xx.cnt`, NXP's settings for rev 0x0b73) and stop on any protection flag or a die over
  100 C; `DLisaAudio.driver` (HakiOS's plug-in ported to 26) in `/Library/Audio/Plug-Ins/HAL`, libdlisa in audiomxd
  and `dlisa_sndtest` in SpringBoard. Kernel `kernel-16k-audio1.bak`, root `920e8d79`, runs A0 to A3:
  [handover-2026-11-16-audio.md](handover-2026-11-16-audio.md).

## 7. NFC, fingerprint, modem (survey and plan only)

- **NFC**: NXP SN100 family (`qcom,sn-nci`) at I2C 0x28 (bus of fragment@43), VEN GPIO 38, FIRM 40, IRQ 41, CLKREQ 39;
  Android: `vendor.nxp.hardware.nfc@2.0`. iOS's NFC (nfcd, Apple's NXP SN-series with Apple firmware and the Secure
  Element tied to SEP for Apple Pay) expects its own stack; a reader-only path would mean writing NCI in the kernel and
  faking nfcd's transport. Not worth it now: weeks, little value.
- **Fingerprint**: FPC (fpc1020 binding, `fpc1540` module), reset GPIO 43, IRQ 34; the sensor's SPI and matching live in
  TrustZone (QSEE TA). iOS's Touch ID wants a Mesa sensor talking to SEP. Infeasible without SEP; a "button only"
  use (presence via IRQ) is possible but pointless. No plan.
- **Modem**: MPSS remoteproc, QMI over QRTR/GLINK, data through IPA. HakiOS has `drivers/modem/` (start of MPSS from
  `modem.mdt`/`bNN` in `/fw`, QMI DMS online, NAS registration and signal into `kern.hakios.cellular_*`, IPA data shown
  as an Ethernet `en1` with DHCP in the kernel). Plan: port `hakios.mpss.c` + `hakios.qmi.c` + `hakios.nas.c` like the
  ADSP was ported (the darwin-lisa ADSP already shares the SMP2P/GLINK/QRTR code), read-only firmware from `/fw`, never
  touching `persist` or `modem` partitions; status in the status bar needs CommCenter, which expects an Apple baseband:
  out of reach; data via `en1` is reachable (about 1 to 2 weeks with the phone). Calls/SMS: no.

## Test plans (autonomous: logs, injected events, states read back)

Common part (as `clk-cc1`): `B="dlisa_unlock=1 dlisa_noidle=1 display=1 console_open=1 lisa=1 keystore=1
init_run=/usr/libexec/usermanagerd:--init cpus=4"`. Driver seconds count from the driver's start (a few seconds after
the kernel's start); screenshot seconds are gpu-quietrun's. Phone in pmOS first, as in the handovers.

### P1: keys, vibration, flashlight (kernel only; the current root is enough for the keys and the drivers)

```sh
E="$B buttons_seq=vu@170,vu@172,vd@176,shot@190,pw@210,wake@222 haptic=5 haptic_seq=2@150,1400@155,10@160 torch=3 torch_seq=1000@235,300@240,0@245"
KERNEL=kernel-16k-periph1.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh periph-p1 "$E" "160 175 195 215 230 250"; bash darwin-lisa/tools/bootos.sh pmos
```

Pass:
- keys: `buttons: the power-on block says type 0x1, subtype 0x9`, `volume up's pin: mode 0, ... enabled, level high`,
  `buttons: step 5: watching power volume-up volume-down`; each `buttons: test press ...`; backboardd
  `dlisa[backboardd]: input: sample N type 4 finger 2` (volume up) etc.; `periph-p1-q2.png` (+175) shows the volume HUD;
  `periph-p1-q3.png` (+195) the screenshot thumbnail; `periph-p1-q4.png` (+215) black (locked), `q5` (+230) the lock
  screen again. A hand on the keys gives `buttons: power PRESSED at ...`.
- vibration: `haptic: chip ID 0x24 (AW8624)`, `haptic: aw8624_haptic.bin: 5348 bytes, checksum 0xe51e, RAM base 0x800`,
  `haptic: waveforms loaded and read back`, `haptic: command 2 played`, `command 1400 played`; no `the chip reports`.
- flashlight: `torch: channel N (this system's); type 0x18, subtype 0x7 ...`, `torch: ON (level 1000 ...)`,
  `torch: ON (level 300 ...)`, `torch: off (level 0 ...)`, no `OFF:` line. If the channel is "NOT this system's",
  nothing was written (that is the answer, not a failure of the run).

### P2: Bluetooth (kernel), one level per boot

```sh
KERNEL=kernel-16k-periph1.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh periph-bt1 "$B bt=1" "200"
# if bt=1 shows SE7 protocol 2, GPLL0 running and the supplies: bt=2, then bt=3
KERNEL=kernel-16k-periph1.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh periph-bt3 "$B bt=3" "240"
```

Pass: bt=1 `look done` (on lisa it also says `SE7's clocks are not all on` and the regulators are not readable from this system: both are findings, not failures; go on to bt=2 when `GPLL0: ... locked` is there); bt=2 turns SE7's clocks on before it reads `SE7: fw revision ... (protocol 2 = UART)`, votes l19b, s1c, l11c on through RPMh (`voted on through RPMh`), leaves the Wi-Fi half's s7b, s1b, s8b alone, then `QCA product ... firmware set 11`, `local version: ... manufacturer 29 (Qualcomm)`;
bt=3 `msbtfw11.mbn downloaded`, `msnv11.bin downloaded`, `QCA setup complete`, `found LE ...` lines (any phone or
headset nearby), `level 3 done`. Any `bt: K: FAILED:` names the step.

### P3: Bluetooth in Settings (virtual controller) and speaker tone

Needs the test root `system-26.apfs.full-9a91ed47` (`tools/flash-root.sh`; rollback int3 122a0d5c). Do not put `ld_disable=` with the `lean3` or `missing` groups on the line: both list `com.apple.bluetoothd`. Line: `"$B dlisa_btvirt=1 bt-transport-override=10 adsp=1 sound=2
touch_seq=tap:56800:5600@200"` (Settings opened by the tap; then Bluetooth row). Pass: `dlisa[bluetoothd]: bt:
sysctlbyname rebound in N slot(s)`, `kern.hv_vmm_present answered 1`, no "No valid serial port found"; a screenshot of
Settings with Bluetooth "On"; `sound: the DSP took N frames a second` near 48000 (silent: amps off at sound=2).

### Results on the phone (2026-11-16, kernel periph1, root 9a91ed47)

- **periph-p1**: flashlight PASSES (channel 151 this system's, ON 1000 / 300 / off, no `OFF:`; `battery -273 C` = no
  battery driver on the line). Keys: 22505 reads, 0 failed, every test press reached backboardd; power locks the
  screen; side + volume up starts `ScreenshotServicesService` (log `DLISA exec 398(ScreenshotServic)` 0.2 s after the
  chord), so SpringBoard sees both keys; the missing thumbnail is the screenshot service's capture, not the keys. No
  volume HUD: SpringBoard shows it when the audio system's volume changes, and audiomxd has no output device here
  (section 6); the volume keys themselves arrive. Vibration FAILED at `after loading: the chip reports 0x30`: 0x20
  under-voltage + 0x10 FIFO almost empty, which Xiaomi's driver only logs (it masks the under-voltage interrupt when
  the chip is idle): periph2 treats only over-voltage, over-current and over-temperature as faults.
- **periph-bt1**: `look done`; SE7's branch off (vote bit 17 = 0, CBCR 0x80004220), RCG at 19.2 MHz, GPLL0 600 MHz
  locked; the regulators are RPMh's (no arbiter channel, or another peripheral there). periph2's bt=2 votes the
  Bluetooth-only rails through RPMh as Linux's rpmh-regulator does.

### Rollback

Kernel: run with the previous `KERNEL=` (`kernel-16k-s14-idle.bak` or `kernel-16k-consolidated-rc3.bak`); the new
drivers do nothing without their boot-args (keys: `buttons=0`). Root: `tools/flash-root.sh` with the previous root.
