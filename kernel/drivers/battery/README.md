# Battery on iOS 26 (lisa)

## What lisa has

On lisa the fuel gauge and the charger belong to the **audio DSP's battery manager**. postmarketOS uses
`pmic_glink` + `qcom_battmgr` (`xiaomi-lisa-mainline/docs/hardware.md`: "Battery, charging | Work | pmic_glink /
battmgr"), with the ADSP firmware `qcom/sm7325/xiaomi/lisa/adsp.mbn` (19.9 MB) and `adsp*.jsn`. Mainline lisa has no
charger or fuel-gauge node on SPMI. The PMIC is PM7325B (the DT uses pm8350b.dtsi), slave 3.

A real percentage, current, temperature and cycle count therefore need the ADSP: the PAS loader, SMEM, GLINK, IPCC
and the battmgr protocol. HakiOS has all of that for redwood (`drivers/audio/hakios.adsp.c`, 121 KB, `adsp=1`), but it
was never brought up on lisa. That is a much bigger port. Note `xiaomi-lisa-mainline/docs/hardware.md:60`: lisa's
firmware reports the charge current **negative** while charging, the opposite of redwood.

## What this driver does instead

`dlisa.battery.c` reads the battery's voltage directly over SPMI, with no firmware:

- PMIC arbiter v5 (core 0x0c440000, cnfg 0x0c40a000, observer 0x0e600000, write channels 0x0c600000), as HakiOS's
  buttons and torch drivers do.
- The PMK8350 ADC7 (slave 0, peripheral 0x31, `qcom,spmi-adc7`). Conversions, in Linux `qcom-spmi-adc5.c` order:
  APP_SID 0x40, EN_CTL1 0x46, DIG_PARAM..DELAY_CTL 0x42..0x45, CONV_REQ 0x47, poll STATUS1 0x08, DATA 0x50/0x51.
  The voltage is `code * 1.875 V / 0x70e4 * prescale`.
- Channels (`dt-bindings/iio/qcom,spmi-adc7-pm8350b.h`, slave 3): `VBAT_SNS` 0x38f (1/3), `USB_IN_V` 0x311 (1/16),
  `VPH_PWR` 0x38e (1/3).
- The charge is **estimated from VBAT** with the xnuloader's table (3300 mV 0%, 3700 10%, 3900 50%, 4200 90%,
  4400 100%). A charger is "there" when USB_IN reads above 4 V. "Charging" is taken from the trend: on a supply,
  charging only while VBAT is not below its value 6 readings (3 min) earlier, else "on a charger, not charging". On
  the phone VBAT fell on USB (4142 -> 4086 mV in 4 min at 7 cores and 2.4 GHz), since nothing manages the charger. Current is 0 and temperature a neutral 25 C, because both are unknown without the gauge.
- Every reading goes to `hakios_battery_report()` -> `HakiosBattery` (`hakios.battery.cpp`, HakiOS's IOPMPowerSource,
  unchanged). powerd reads it, and SpringBoard's battery follows. The first reading creates the service.
- A VBAT outside 2.5..4.6 V is treated as a wrong conversion: nothing is reported, and the log says so.

The ADC register offsets and the scaling come from the Linux driver, from memory. No Linux tree is on this machine.
**They are unverified on the phone.** The driver logs everything needed to check them: the ADC's type and subtype,
which system owns its arbiter channel, a dump of registers 0x00..0x07, 0x40..0x47 and 0x50..0x51 as found, and every
conversion's raw code and status.

## Files and wiring

| File | What |
| --- | --- |
| `dlisa.battery.c` | the SPMI/ADC reader (bsd/dev), `hakios_battery_init/_report/_temperature` |
| `hakios.battery.cpp` | HakiOS's `HakiosBattery : IOPMPowerSource` (iokit/Kernel), copied unchanged |
| `battery-wire.py` | tag `hakios_battery` (option `HAKIOS_BATTERY`, drops the hakios.stubs.c no-ops) |

## Boot-args

- `battery=1` turns it on (off by default).
- `battery_secs=N` sets the time between readings (default 30 s).
- `battery_adc=0` makes it read only: the channel table and the registers, no conversion and no write.
- `battery_dump=1` dumps the ADC's registers after every conversion.

Sysctls: `kern.hakios.battery_soc`, `battery_voltage_mv`, `battery_usb_mv`, `battery_vph_mv`,
`battery_charger_present`.

## Log markers

- `battery: PMIC arbiter version 0x5..., N channels`
- `battery: channel N is the PMK8350's ADC (0x...), written by system S` (S must be 0 for conversions)
- `battery: the ADC (channel N, ours to write): type 0x.. subtype 0x..`
- `battery: adc as found: 00: ..  40: ..  50: ..`
- `battery: VBAT_SNS: code 0x.... status 0x.. -> NNNN mV`
- `battery: VBAT NNNN mV -> NN% (estimated from the voltage), USB in NNNN mV (...), VPH NNNN mV`
- `HakiosBattery: the battery's power source is up: NN%, charging`

## Status on the phone (2026-11-10)

```
battery: PMIC arbiter version 0x50020000, 512 channels
battery: channel 101 is the PMK8350's ADC (0x1003100), written by system 0
battery: the ADC (channel 101, ours to write): type 0x8 subtype 0x44
battery: adc as found: 00: 00 00 00 00 08 44 00 00  40: 03 00 18 80 45 01 00 00  50: 27 20
battery: VBAT_SNS: code 0x5324 status 0x1 -> 4142 mV
battery: USB_IN_V: code 0x1210 status 0x1 -> 4800 mV
battery: VPH_PWR: code 0x52d0 status 0x1 -> 4126 mV
battery: VBAT 4142 mV -> 82% (estimated from the voltage), USB in 4800 mV (a charger or a computer), VPH 4126 mV
HakiosBattery: the battery's power source is up: 82%, charging
```

The register map and scaling were right the first time: VBAT and VPH agree within 20 mV, USB reads 4.8 V. The status
bar shows the battery (`research/hackios-xnu-kernel/logs/drv-run2-shots/fullroot-shot-180.png`, green with the bolt;
the iPhone layout shows no percentage on the cover sheet). Over 15 minutes on USB VBAT went 4142 -> 4065 mV: the
phone does not charge under iOS, hence the trend rule above (the newer kernel says `status 3`, "on a charger, not
charging").
