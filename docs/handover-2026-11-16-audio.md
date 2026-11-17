# Handover 2026-11-16: speakers heard from iOS (host only)

Nothing was run on the phone and nothing was written to it. The camera was set aside on the owner's word (nothing
started there; the state stays [camera.md](camera.md)). Details: [audio-ios.md](audio-ios.md) (the iOS 26 side, what
the 23F84 binaries say), [kernel/drivers/audio/README.md](../kernel/drivers/audio/README.md) (the kernel side,
`sound_tune`, the status rules).

## Built, NOT written

| Piece | sha256 |
| --- | --- |
| kernel `research/hackios-xnu-kernel/kernel-16k-audio1.bak`: the shared tree as found at 19:50 (unixnu4 + the brightness worker's wires of bright1, not reverted) + this tree's audio wire (`audio-build.sh`: lock, only the audio wire, symbols of the audio and of unixnu4's `gpu_irq` checked, the new strings checked) | `d7f5717d56f2188220ee5b8626a4e51766c6a132930f4d1e409ff442e61d9b83` |
| root `userland-26/out/system-26.apfs.full-920e8d79`: int14 215d11f6 + `DLisaAudio.driver` (eacefe2a, Info.plist 5ccb239c) + libdlisa of HEAD c6c578a (24c71c94: audiomxd probe, `dlisa_sndtest`) + `dlisa-inject.conf` with audiomxd (c9009dc9). `build-root-audio.sh`; the sysroot's stray `usr/libexec/backboardd.a2s` (newer than int14, not in it) left out. Read back: every file above; census 61220 files (int14 + 2), 24196 dirs, 0 bad. 4,999,610,368 B | `920e8d794b561399a5fe1f457a94773d32b243d9f7d69db40593c1067e96f2fa` |

## What changed

- **TFA9873 with Xiaomi's tuning** (`sound_tune=1`, default): the stock container is for a 4 x 32-bit TDM bus at 96 kHz
  with NXP's protection algorithm in the ADSP; what fits pmOS's 2 x 16-bit at 48 kHz is taken (max coil current
  DCMCC 9, OCP scaling of amplifier and boost, boost voltage and trips, NXP's optimal settings for rev 0x0b73 behind the
  keys), the bus format, PLL and gain are not (`sound_tune=2` adds the gain, `0` is pmOS only).
- **Safety**: status registers decoded with NXP's field names; over-temperature, amplifier or boost over-current,
  over-voltage, or a die over 100 C stop both amps for the run; temperature, battery reading, clipping and
  under-voltage logged.
- **iOS**: `DLisaAudio.driver` (HakiOS's plug-in for 26: `Speaker` to `/dev/hakios-audio` with a volume control that
  goes to the kernel, `Hawking`, `Actuator`, `Codec`), libdlisa in audiomxd (product type, AcousticID, closed-loop
  haptics), `dlisa_sndtest` in SpringBoard.

Commits: 340fe02 (TFA tuning and status), ed75cec (plug-in, audiomxd probe), c6c578a (`dlisa_sndtest`), 1164d89 (root
script, docs), this document.

## Test plan (autonomous: logs and screenshots)

Common part (as P1): `B="dlisa_unlock=1 dlisa_noidle=1 display=1 console_open=1 lisa=1 keystore=1
init_run=/usr/libexec/usermanagerd:--init cpus=4"`. Phone in pmOS first. `buttons_seq` seconds count from the keys'
driver start (about 20 s after the kernel's); `dlisa_sndtest` seconds from SpringBoard's start.

### A0: the amplifiers with the tuning, the kernel's tone (any root; kernel audio1)

```sh
KERNEL=kernel-16k-audio1.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh aud-a0 "$B adsp=1 sound=3 sound_amps=2" "120"; bash darwin-lisa/tools/bootos.sh pmos
```

Pass (grep `sound:`): `adsp: 11: THE AUDIO SERVICES ARE UP`; `sound: level 3 (... sound_tune 1 ...)`; `sound: tfa: 0x35
(bottom, right): keys 1 and 2 opened`; `Xiaomi's tuning (NXP's 0x0b73 settings and the container, profile 'stereo'): N
registers as written, 0 not; gain TDMSPKG g`; `THE TONE PLAYS NOW`; `... while playing: ... clocks, pll, ... amp on ...;
T C` with no `FAULT`; `LEVEL DONE`; `while on: looked at N times, hottest T C, clipping in 0 looks`. Audible: a 1 kHz tone
from the bottom speaker, about 3 s, at -32 dBFS (quiet). If a line says `READS` or `FAULT`, run the same with
`sound_tune=0` (pmOS only) to tell the tuning from the path.

### A1: sound from iOS (root 920e8d79, kernel audio1)

Flash the root (`tools/flash-root.sh userland-26/out/system-26.apfs.full-920e8d79`; rollback int14 215d11f6). 417
characters with the quietrun's prefix:

```sh
KERNEL=kernel-16k-audio1.bak bash darwin-lisa/kernel/drivers/gpu/gpu-quietrun.sh aud-a1 "$B adsp=1 sound=4 sound_amps=2 dlisa_oslog=audiomxd dlisa_sndtest=110,125:alarm,140:Tock buttons_seq=vu@200,vu@201,vu@202,vd@215" "150 221 224 240"; bash darwin-lisa/tools/bootos.sh pmos
```

Read, in this order (each its own mark):

| Mark | Where | Pass |
| --- | --- | --- |
| identity | `dlisa[audiomxd]: audio: MobileGestalt product type 0x..., AcousticID n, closed-loop haptics h` | the line (write the numbers down: they decide the tunings folder) |
| plug-in loaded | `dlisa-audio[audiomxd ...]: factory called`, `loaded by the HAL: on; devices: Speaker, Hawking, Actuator, Codec; /dev/hakios-audio: open` | both; the process named is where the HAL runs |
| routed | `a client of Speaker`, `Speaker: I/O starts`, `the volume is ...` | present; otherwise the `asked of object N (Speaker) what it does not have` lines and audiomxd's `oslog[audiomxd]` lines (`concrete route`, `Unsupported product type`, `Speaker Protection`, `Tunings`) are the answer |
| played | `dlisa[SpringBoard]: sndtest: playing .../ReceivedMessage.caf`, `... done after N ms`; `dlisa-audio[...]: Speaker: N frames in the last second ..., peak P` with P > 0 | frames with a peak after each sndtest line |
| kernel | `sound: Darwin plays: the amplifiers are powered up`, the TFA status lines without `FAULT`, `three seconds of silence: the amplifiers are powered down`, `sound_status` underflows 0 | present; heard from the bottom speaker |
| volume, HUD | `sound: Darwin sets the volume ...` or the plug-in's peak changing after `vu`; screenshots `aud-a1-q2.png`/`q3` (+221, +224) | the volume HUD on screen |

If `sndtest ... done` comes with no plug-in frames, the system sounds may be silenced (no ringer switch on lisa):
the HUD and `vu` presses still say whether a route exists.

### A2: if A1 does not route (one line changed each time)

- `dlisa_audio_devs=speaker,hawking` (no actuator, no codec), then `dlisa_audio_devs=speaker` alone: which companion
  VirtualAudio wants.
- `dlisa_audio_devs=speaker,receiver,hawking,codec,actuator`: the receiver too (VirtualAudio's
  `Speaker and receiver are mutually exclusive` logic).
- If audiomxd says its tunings are missing for AcousticID n: the next root carries `/Library/Audio/Tunings/AID2028`
  (the iPad's VAD and HAL, 0.6 MB, from the IPSW) and a link `AID<n>` to it.

### A3: both speakers, then louder (only after A1 is clean)

`sound_amps=3` (the top unit too: it is also the earpiece), then `sound_atten=9`; never under 6. Watch the hottest
temperature and the clipping count of each run.

## Rollback

Kernel: `kernel-16k-unixnu4.bak` (without the brightness wires) or `kernel-16k-bright1.bak`; every audio change is inert
without `sound=`. Root: int14 `215d11f6`; with this root, `dlisa_audio=0` makes the plug-in publish nothing.
