# Flashlight (lisa, iOS 26)

The rear LED is channel 0 of the PM8350C's flash LED module (SPMI slave 2, registers 0xee00, PPID 0x2ee). The stock
lisa overlay drives the torch as `torch_0` through `switch_0` (`qcom,led-mask` 0x01) for every rear camera, exactly as
redwood's does, so `dlisa.torch.c` is HakiOS's `drivers/camera/hakios.torch.c` on top of `../rtc/dlisa.spmi.c`.
Register layout checked against Linux 7.2.9 `leds-qcom-flash.c` (`mvflash_4ch_regs`). Current: 125 mA (Android's torch
on redwood, ITARGET 9 in 12.5 mA steps); lisa's tree allows 500 mA for torch_0.

## Boot-args

- `torch=1` look (reads only), `torch=2` on once for `torch_secs` (default 2), `torch=3` the light for Darwin.
- `torch_max_s=N` longest stretch on (default 120 s, 1..600), then off until 0 is asked for.
- `torch_seq=L@S,...` (torch=3) levels for tests: `torch_seq=1000@200,0@205`.

## Interfaces (torch=3)

- `/dev/hakios-torch`: write a 32-bit level 0..1000 (0 off); read what is lit.
- `kern.hakios.torch_level` (RW int, any user): the same level. Reads back what is lit.
- `kern.hakios.torch_status`: `level L lit N wanted N times N cut N broken B`.

## Guards

Only six registers of channel 0 may be written, each read back; writes only if the arbiter says EE 0 owns the channel;
ten times a second while lit: module fault, open/short, over-temperature, VPH droop, the PM8350C temperature alarm, the
battery at 45 C. Any of them: off for the rest of the run.

## Log markers

`torch: channel N (this system's); type 0x18, subtype 0x7, ... (the 4-channel module)`, the register dump, `torch: the
light is Darwin's`, `torch: ON (level 1000 of 1000 ...)`, `torch: off (level 0 ...)`.

## Not verified (needs the phone)

Whether EE 0 owns the module's arbiter channel on lisa (the look prints it; without it nothing is written), and the
light itself.
