# Side and volume keys (lisa, iOS 26)

```
PMK8350 PON (KPDPWR_N, RESIN_N) + PM7325 GPIO6   (SPMI, read only, every 16 ms)
  -> dlisa.buttons.c          button records of /dev/hakios-touch (type 4 down / 5 up; finger = 1 power, 2 vol+, 3 vol-)
  -> libdlisa in backboardd   "darwin-lisa buttons" virtual HID service, consumer page: Power 0x30, Vol+ 0xE9, Vol- 0xEA
  -> backboardd               lock / wake, volume HUD, side + volume up = screenshot, long side = Siri / power-off slider
```

The user-space half was already there (`userland/inject/dlisa-inject.c`, `sendButton()`); only the kernel driver was
missing (`hakios_buttons_init()` was the no-op of `hakios.stubs.c`).

## Hardware

Both device trees agree (pmOS `sm7325-xiaomi-lisa.dtb`, the stock lisa dtbo overlay):

| Key | Input | SPMI | Register |
| --- | --- | --- | --- |
| Power (side) | KPDPWR_N of the PMK8350 power-on block | slave 0, PPID 0x013 | 0x1310 bit 7 (RT_STS) |
| Volume down | RESIN_N, same block | slave 0, PPID 0x013 | 0x1310 bit 6 |
| Volume up | GPIO6 of the PM7325, pull-up, low when pressed | slave 1, PPID 0x18d | 0x8d10 bit 0 |

Same layout as redwood (POCO X5 Pro): `dlisa.buttons.c` is HakiOS's `drivers/input/hakios.buttons.c` (proven by hand on
redwood) on top of the shared arbiter code `../rtc/dlisa.spmi.c`. Nothing is written to a PMIC.

## Boot-args

- `buttons=1` on; default: on whenever `touchscreen=` is on, which `lisa=1` gives. `buttons=0` off.
- `buttons_steps=N` stop after step N (1-2 channels, 3 identify, 4 inputs, 5 watch).
- `buttons_seq=K@S,...` test presses posted exactly as the keys' own, S seconds after the driver starts. K: `pw`, `wake`
  (power 150 ms), `pwlong` (power 3.5 s), `vu`, `vd` (volume 150 ms), `shot` (power + volume up together).
- `press_power=S` (`press_power_ms=`): HakiOS's old test, power at S and S+10 s.

## Sysctls

- `kern.hakios.buttons_state`: bit 0 power, 1 volume up, 2 volume down, held now.
- `kern.hakios.buttons_presses`: `power N volume-up N volume-down N injected N reads N failed N`.
- `kern.hakios.buttons_inject` (write): 1 power, 2 volume up, 3 volume down, +100 held 3.5 s.

## Log markers

`buttons: the power-on block is channel N`, `buttons: the power-on block says type 0x1, subtype 0x9`, `buttons: volume
up's pin: mode 0, pull ..., enabled, level high (released)`, `buttons: step 5: watching power volume-up volume-down`,
then `buttons: power PRESSED at ...` / `released at ..., after N ms`, and `buttons: test press ...` for injected ones.
backboardd: `dlisa[backboardd]: input: sample N type 4 finger 1` (first 64 samples).

## Files

| File | What |
| --- | --- |
| `dlisa.buttons.c` | the driver (`hakios_buttons_init()`, tag `hakios_buttons`, option `HAKIOS_BUTTONS`) |
| `buttons-wire.py` | the wiring; a second `files` line makes `dlisa.spmi.c` visible for this tag too |
