# Speakers on iOS 26 (lisa): ADSP APR path + TFA9873

Status (2026-11-16): written and checked offline only. Nothing here has run on the phone yet.

A PCM stream played through the audio DSP to the two speaker amplifiers, the way pmOS's Linux 7.2.9 plays them on
lisa, ported from HakiOS's `drivers/audio/hakios.sound.c` (proven on redwood, same SM7325 and DSP family). Gated by
boot-args. `/dev/hakios-audio` (already in the kernel: `../hakios.audio.c`, standard) is the user-space interface.

## Files

| File | What |
| --- | --- |
| `dlisa.sound.c` | `sound=1..4`: MI2S pins, AFE port, SMMU context for stream 0x1801, ADM path, ASM stream, test tone, sink of `/dev/hakios-audio`, sysctls. Defines `hakios_sound_dsp_up/_down/_quiesce()` |
| `dlisa.tfa9873.c`, `.h` | The two TFA9873 over bit-banged I2C (GPIO 12/13), register setup copied from pmOS's `tfa9872.c` |
| `audio-wire.py` | Wires the files into the tree (see below) |

## Hardware and numbers (all from pmOS's DTB, Linux 7.2.9, and pmOS's own modules)

- **Path:** ASM session 1 stream 1 (16-bit PCM, topology 0x10c68), then ADM COPP (playback, topology 0x10312, 2 ch,
  16 bit, 48 kHz), then AFE port **0x1000 PRIMARY_MI2S_RX**. Same as pmOS UCM `PRI_MI2S_RX Audio Mixer MultiMedia1`.
- **AFE:** bit clock `PRI_MI2S_IBIT` (0x100) at **1.536 MHz**, attribute COUPLE_NO, root 0. `AFE_PARAM_ID_I2S_CONFIG`:
  16 bit, channel mode SD1 (`qcom,sd-lines <1>`), stereo, WS internal, 48 kHz. Clock, format and the S16_LE fixup were
  read from pmOS's `snd-soc-sm8250.ko` by disassembling it: clock 0x100 at 0x177000 Hz, cpu fmt BP_FP, codec fmt
  I2S|BC_FC.
- **Pins:** GPIO 97 sck, 98 data0, 99 data1, 100 ws. Function 1, 8 mA, no pull, output high, as pmOS's
  `pri-mi2s-*-active`. They are put back as found afterwards.
- **SMMU:** apps_smmu stream **0x1801** (pmOS `q6asm-dais iommus`), the same as redwood. The DSP gets addresses with
  SID bits `1 << 32` (q6asm-dai.c). A free SMR and context are taken under `hakios_smmu_lock`, with our page tables at
  IOVA 0x10000000, the same way as HakiOS and `../adsp/dlisa.fastrpc.c` (0x1805).
- **Amps:** I2C QUP SE3. SDA is GPIO 12 and SCL is GPIO 13 (`qup-i2c3-data-clk`), at about 100 kHz.
  - 0x34 "Earpiece": the top unit, slot 0 (left), reset GPIO 2.
  - 0x35 "Speaker": the bottom unit, slot 1 (right), reset GPIO 6.
  - The reset is active high. pmOS never drives it, so this driver only reads it unless `sound_amp_reset=1`.
  - The stock tree also has `spk-sw-gpio` 101 on 0x34. It is logged, never driven.
  - Registers use 8-bit addresses and 16-bit values, MSB first. Writes are exactly pmOS's probe: soft reset, DCDC,
    DPSA off, 2x16-bit slots, SPKS=channel, no sense, MANSCONF. After that come DAPM power (PWDN) and mute (AMPE).
  - The TFA9873 has no volume register in pmOS's driver, so the level is set on the samples.

## Boot-args

All of them need **`adsp=1`**. `hakios.adsp.c` calls `hakios_sound_dsp_up()` only after its step 11 logs
`THE AUDIO SERVICES ARE UP`. If that line is missing, the sound never starts. `adsp_apr_wait=S` (default 10) is how
long it asks.

| Boot-arg | Meaning |
| --- | --- |
| `sound=1` | Port only: pins, clock, I2S config, port start. GPIO 97/100 are sampled for toggling, held for `sound_secs` (default 3, max 10), then everything is stopped. The amps' bus is not touched |
| `sound=2` | Adds the SMMU context, ASM map, ADM open, ASM open, routing, format, run, and a 1 kHz tone at -20 dBFS for `sound_secs` (max 3). The amps are only read (revision, status), never written. Silent |
| `sound=3` | Adds the amps: pmOS setup once the clock runs, power up, unmute. The tone fades in and out over 100 ms each, at `-20 - sound_atten` dBFS (default -32 dBFS), then mute and power down. **Audible.** `sysctl kern.hakios.sound_tone=MS` replays the whole sequence (200..3000 ms) |
| `sound=4` | Serves Darwin. Port and stream stay up until the run ends. `/dev/hakios-audio` writes go through a DC blocker (one pole, about 40 Hz), then gain (`sound_atten` + Darwin's `HAKIOS_AUDIO_SET_VOLUME`, ramped), then a clip. The amps power up and fade in on the first non-silent write and power down after 3 s of silence or on mute. `sound_tone=MS` injects the tone as if Darwin wrote it |
| `sound_atten=DB` | Attenuation under full scale (6..60, default 12) at levels 3 and 4 |
| `sound_amps=1\|2\|3` | 1 = top 0x34 only, 2 = bottom 0x35 only, 3 = both (default). Use 2 for the first audible test (loudspeaker only, receiver untouched) |
| `sound_amp_reset=1` | Pulses each reset pin high for 2 ms, then drives it low, before probing. Use only if a chip does not answer |
| `sound_dcdc=0` | Clears the DC-DC bit. The default 1 is what pmOS intends with no DC-DC properties in the tree (see risks) |
| `sound_pause=MS` | Pause after each "about to" announcement so the log is out before the step runs (default 300) |

Suggested sequence: `adsp=1 sound=1`, then `sound=2`, then `sound=3 sound_amps=2`, then `sound=3`, then `sound=4`.

## Interfaces

- `/dev/hakios-audio` (`../hakios.audio.c`, `sys/hakios.audio.h`): write 16-bit signed LE stereo frames at 48 kHz.
  Writes never block, and what the ring (0.68 s) cannot take is dropped. `ioctl(HAKIOS_AUDIO_SET_VOLUME, {below
  (1/8 dB), muted})`. At `sound=4` this driver is the sink. The user-space HAL writes here in real time, the same way
  as HakiOS's `audio_plugin.c`.
- `kern.hakios.sound_status`: level, state, amp state, DSP period count and errors, underflows, tones played, frames
  from Darwin, last action.
- `kern.hakios.sound_tone` (RW int, root only): write a duration in ms to play the test tone (levels 3 and 4). Reading
  it returns the number of tones played.

## Log markers (grep `sound:`)

- `adsp: 11: THE AUDIO SERVICES ARE UP`: the precondition.
- `sound: level N (...)`, then each command as `sound: > <what> (opcode ...)` followed by `done`, `REFUSED: <error>`,
  `NO ANSWER`, or `AN ANSWER NOT EXPECTED`. Any line other than `done` stops the level there.
- `THE MI2S BIT CLOCK AND WORD SELECT RUN at the pins` or `THE PINS DO NOT SHOW A RUNNING MI2S CLOCK`.
- `THE AUDIO STREAM GOES THROUGH CONTEXT n`, `our 32 KB at 0x110000000 are the DSP's handle`,
  `the device path is open: COPP n`.
- `... the DSP took N frames a second`: should be about 48000. Also `THE DSP HAS GIVEN NO PERIOD BACK`,
  `THE SMMU HAS REFUSED A REQUEST`.
- `sound: tfa: 0x35 (bottom, right): revision 0x0b73 (TFA9873)`, `... after the soft reset SYS_CTRL0 is ...`,
  `... SYS_CTRL0 x, status 0x10 y, 0x11 z` (raw, logged when it changes).
- `THE TONE PLAYS NOW`, then `LEVEL DONE` or `STOPPED SHORT; last: ...`.
- `WHAT DARWIN PLAYS GOES TO THE SPEAKERS`, `Darwin plays: the amplifiers are powered up`,
  `three seconds of silence ...`.
- `BOTH AMPLIFIERS OFF`: an amp fault or deviation was detected and the amps are given up for the run.

## Safety limits

- At level 3 the tone peaks at -20 dBFS less `sound_atten` (at least 6 dB), lasts 3 s at most, has a 100 ms fade at
  each end and no DC.
- At level 4 Darwin's samples are attenuated by at least 6 dB, DC-blocked, ramped over 400 ms, and clipped.
- The amps are unmuted only while the MI2S clock runs. They are muted and powered down before the stream and the port
  stop, after 3 s of silence, on mute, on any I2C NACK or read-back mismatch, and when SYS_CTRL0 changes by itself
  (checked every 200 ms). After any of those, they stay off for the rest of the run.
- Panic path: `hakios_sound_quiesce()` powers both amps down (spin-only I2C, no lock) and puts the SMMU context back.
- Every hardware write is logged before it is made, and every DSP answer is checked.

## Wiring

`audio-wire.py` (idempotent, `XNU_WORK=<dir>` for a dry run). Run it after `hakios-wire.py` and `adsp-wire.py`. It:

- copies the three files to `bsd/dev/`;
- adds `bsd/conf/files` lines `optional dlisa_sound`;
- adds options `DLISA_SOUND` and **`HAKIOS_SOUND`** under tag `dlisa_sound`;
- adds the tag to `BSD_DEV`.

`HAKIOS_SOUND` drops the empty `hakios_sound_*` stubs in `hakios.stubs.c`. It is used nowhere else in the tree. No
edit to `hakios.adsp.c` or `hakios.stubs.c` is needed, because `hakios.adsp.c` already calls `hakios_sound_dsp_up()`
after APR is up, `hakios_sound_dsp_down()` from `hakios_adsp_stop()`, and `hakios_sound_quiesce()` from
`hakios_adsp_quiesce()`. It refuses to run (`[FAIL]`) if those calls, the stubs, `HAKIOS_ADSP`, or
`hakios_smmu_lock` are missing.

## Verified offline

- `tools/ksyntax.py` with the 26 build's flags (`hakios.adsp.o.json`, `dlisa.rtc.o.json`): 0 errors. Only
  `-Wunsafe-buffer-usage` warnings remain.
- The wire script was dry-run twice on a copy of the tree files. The first run applied everything and the second
  skipped everything. The resulting diff is only the files lines, two options, and the tag.
- Host test of the sample math: dB-to-Q15 table within 0.2 % for 0..90 dB, tone peak 3277 (-20 dBFS) and 823 at
  12 dB attenuation, fades start and end at 0, the DC blocker takes a constant input to 0 within one period, and the
  gain ramp and clip behave.
- Opcodes and payload layouts were checked against Linux 7.2.9 `q6afe.c`, `q6adm.c`, `q6asm.c`, `q6asm-dai.c` and
  `q6afe-dai.c`. Clock, format and port came from the disassembly of pmOS's `snd-soc-sm8250.ko`. Pin functions came
  from `pinctrl-sc7280.c`.

## Needs the phone (not verified)

- Whether lisa's APR audio services come up at all with our ADSP bring-up (step 11). The audio PD (`avs/audio`) may
  need the PDR/servreg handshake that Linux does, and that handshake is not implemented here.
- Whether the AFE accepts the I2S config at its exact 20 bytes. Linux sends `sizeof(union afe_port_config)` (larger),
  and HakiOS sent the exact size for TDM. If the result is `REFUSED: bad parameter`, pad it to the union size.
- Whether the apps SMMU has a free SMR and context, and whether stream 0x1801 is untouched by the boot chain on lisa.
- Whether the TFA9873s answer at 0x34/0x35 with their reset as U-Boot left it. The revision should be 0x0b73.
- Whether the TFA, with only pmOS's register set (no NXP container), plays cleanly on lisa through this path. pmOS
  plays this way, but how well it sounds there is unknown here.

## Risks and cost from here to iOS audio

1. **APR not up (largest unknown).** If step 11 never reports up, nothing here runs. The fix may be the PDR/servreg
   locator over QRTR (as Linux's `pdr_interface`/`qcom_pdr_msg`). Cost: medium, about a day, host-testable like the
   SSC client.
2. **DC-DC bit.** pmOS's `tfa987x_setup_dcdc()` reads four of its five properties into uninitialised locals. With
   `CONFIG_INIT_STACK_ALL_ZERO` it returns true (DCDC=1), otherwise it is undefined. Default here is 1, and
   `sound_dcdc=0` is the fallback.
3. **No speaker protection.** Android runs NXP's TFA DSP algorithm (`tfa98xx.cnt`, calibration, excursion and thermal
   models). pmOS runs none, and neither does this driver. Protection here is only gain (6 dB minimum, 12 dB default),
   a DC blocker, and the chips' own OTP, OCP and UVP. Status 0x10/0x11 are logged but not decoded, because the
   TFA9873 bit layout is unverified. Long loud playback could heat the voice coils, so keep `sound_atten` at 12 or
   more until the status bits are decoded.
4. **The top unit is also the earpiece receiver.** pmOS drives it as a speaker on slot 0. Whether the stock
   `spk-sw-gpio` 101 changes its path is unknown. `sound_amps=2` keeps it out.
5. **Latency and glitches.** There are 8 periods of 20 ms (160 ms) queued at the DSP, and a 2 ms poll in the feeder.
   The amps start about 10 to 20 ms after the first non-silent write, so the first 20 ms are faded in. An underflow
   shows as `underflows` in the status.
6. **Bit-banged I2C.** Each register access takes about 0.5 ms of spinning. The watcher reads 3 registers per amp
   every 200 ms while playing, which is fine. Nothing else in the kernel uses SE3.
7. **User space (researched separately).** The kernel side is done once level 4 works. The HAL needs an audio-server
   plug-in that opens `/dev/hakios-audio`, writes S16LE stereo 48 kHz frames from its real-time IO thread
   (non-blocking), applies volume itself or uses `HAKIOS_AUDIO_SET_VOLUME`, and writes nothing (or silence) when idle.
   This is HakiOS's `audio_plugin.c` model. Getting iOS 26's audio server to load such a plug-in is the bigger cost
   and is outside this directory.
