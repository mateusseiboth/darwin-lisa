# Vibration motor (lisa, iOS 26)

lisa's motor is driven by an Awinic **AW8624** (chip ID 0x24, I2C 0x5a), not by the PMIC's haptics and not by the
AW86927 redwood has. Source: the stock lisa dtbo overlay (`aw8624_haptic@5A` on `qupv3_se4_i2c`, reset GPIO 35, irq
GPIO 18, `vib_effect_id_boundary` 10) and Xiaomi's driver (MiCode `lisa-r-oss`,
`drivers/input/misc/aw8624_haptic/`). pmOS has no driver for it (its dts leaves a comment at that address).

| | |
| --- | --- |
| I2C | QUP SE4 pins, GPIO 16 data, 17 clock, driven by hand (taken per transaction, given back) |
| reset | GPIO 35, high = running |
| waveforms | `aw8624_haptic.bin` (stock `/vendor/firmware`, 5348 B, checksum 0xe51e, RAM base 0x0800): 1..10 short, 11 looped |

## Boot-args

- `haptic=1` look at the pins; `haptic=2` reset released, chip ID; `haptic=3` configured + waveforms loaded and read
  back; `haptic=4` one effect (`haptic_effect=N`, `haptic_secs=S`); `haptic=5` the motor for Darwin.
- `haptic_seq=CMD@S,...` (haptic=5) commands for tests: `haptic_seq=2@200,1400@205`.

## Interfaces (haptic=5)

- `/dev/hakios-haptic`: write 32-bit commands: 0 stop, 1..10 a short effect, 1000 + MS a vibration of MS ms (at most
  3000).
- `kern.hakios.haptic_play` (RW int, any user): the same command; reads how many were played.
- `kern.hakios.haptic_status`: `level L ready R broken B played N faults N flags 0x..`.

## Firmware

`aw8624_haptic.bin` is built into the kernel from this PC's stock image (`../periph-fw/periph-fw.py header`, put in the
tree by `haptic-wire.py` with `PERIPH_FW_HEADER=`), else read from `fw/` of the RAM image. Never in git.

## Not done / not verified

The f0 calibration (TRIM_LRA stays 0): effects play, a little off resonance. The I2C transfers, the chip ID and the
RAM load need the phone; `haptic=2` and `haptic=3` print each step.
