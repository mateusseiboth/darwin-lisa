# Touch on iOS 26 (Goodix gt9897, lisa)

The path is the one HakiOS used on iOS 16.5, carried over:

```
gt9897 (SPI on SE13, bit-banged)  -> hakios.goodix.c (kernel thread, polls the INT line every 8 ms)
  -> hakios_touch_post()          -> /dev/hakios-touch (hakios.input.c, a queue of struct hakios_touch)
  -> libdlisa inside backboardd   -> a virtual IOHID service "darwin-lisa touch screen" (digitizer, built in,
                                     display integrated) -> digitizer hand + finger events
  -> backboardd's HID event system -> SpringBoard / UIKit
```

There is no IOHIDFamily driver for the screen: backboardd is the HID event server, and the service is created
from inside it (`IOHIDVirtualServiceClientCreateWithCallbacks`, private IOKit API, looked up with dlsym; every
function used is still exported by iOS 26's IOKit). See `userland/inject/dlisa-inject.c`, `startTouchScreen()`.

## Files

| File | What |
| --- | --- |
| `../hakios.goodix.c`, `../goodix-wire.py` | the controller driver (tag `hakios_goodix`, already wired) |
| `dlisa.touch_synth.c`, `touch-wire.py` | test hook: a synthetic swipe up through /dev/hakios-touch |
| `userland/inject/dlisa-inject.c` | the backboardd side (HID service), built by `inject/build-inject.sh` |

## Boot-args

- `touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix touch_goodix_a=1` turn the controller on. Both of the last
  two are **required on lisa**:
  - `touch_chip=goodix`: lisa's strap pins read like redwood's FocalTech variant (`ID gpio96=1 gpio68=0: selected
    focaltech`, then `no focaltech touch screen`), so the chip has to be named.
  - `touch_goodix_a=1`: the gt9897 is Goodix Berlin "variant A" (4 dummy bytes, fw-version at 0x1000C, ic-info at
    0x10068, and the panel's 1080x2400 as the coordinate range). Without it the probe fails.
- `touch_synth=SECS` posts one swipe up from the home bar SECS seconds after bsd_init; `touch_synth_every=SECS`,
  `touch_synth_count=N`, `touch_synth_steps=N`, `touch_synth_tap=1` (a tap first), `touch_synth_tap=2` (a tap only),
  `touch_synth_x=N touch_synth_y=N` (where the tap lands; the cover sheet's (i) is 58060, 62030).
- `touch_seq=G@S,G@S,...` named gestures, each S seconds after bsd_init, in order (independent of `touch_synth`):
  `nc` swipe down from the top-left edge (Notification Center), `cc` swipe down from the top-right edge (Control
  Center on the split status bar; on today's classic layout it opens Notification Center too), `sw` swipe up from the
  home bar held 1.5 s before lifting (the app switcher; on the classic layout the bottom edge is Control Center), `up`
  swipe up from the home bar (home, or dismiss what is open), `tap` a tap at the middle. Kernel log:
  `touch_synth: seq nc posted (x, y) -> (x, y), 25 moves, held 0 ms`, then `touch_synth: seq done`.
  Example (GPU runs): `touch_seq=nc@200,up@235,cc@270,up@305,sw@340`.
- `touch_lat=1` latency lines; `touch_poll_us=N` how often the INT line is looked at.

## Log markers

- kernel: `goodix: ... chip "BERLIN", firmware "9897T" ...`, `goodix: goodix touch screen ready; the interrupt line
  is looked at every 8000 us`, then once a minute `goodix: so far N reports in M looks (B bad), F finger positions`.
- kernel: `touch_synth: swipe up #N posted ...`
- backboardd: `dlisa[backboardd]: input: "darwin-lisa touch screen" opened by the event server (attempt N)`,
  `input: touch screen service 0x..., buttons 0x...`, `input: touch detection mode 0x1`, and the first 64 samples
  `input: sample N type T finger F at X Y`.

## Hand test (the user, at the phone)

Boot (phone in the xnuloader, PC on USB):

```
cd ~/Dev/mobile/research/hackios-xnu-kernel
KERNEL=kernel-16k-drivers-ath11k.bak XL_SECS=1500 XL_POLLS=700 XL_SHOT_EVERY=30 XL_EXTRA_ARGS="display=1 console_open=1 \
  keystore=1 wlan_stub=1 init_run=/usr/libexec/usermanagerd:--init init_run_secs=60 cpufreq=1 cpufreq_max=2400 cpus=7 \
  touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix touch_goodix_a=1 battery=1" \
  ./run-fullroot.sh /home/mateusseiboth/Dev/mobile/userland-26/out/system-26.apfs.trim
```

1. Wait for the cover sheet ("Swipe up to open" and the home bar), about 2 to 4 minutes after the upload.
2. Put a finger on the **home bar** (the white line at the very bottom), and swipe **up** quickly to the middle of
   the screen.
3. Expected: the cover sheet slides up and away, and SpringBoard shows the (empty: the USB image has no
   /Applications) home screen. A swipe down from the top-left edge brings the cover sheet back.
4. Also try a tap on the (i) button at the bottom right.

What to look for in `logs/fullroot.log` afterwards: `goodix: goodix touch screen ready`, `dlisa[backboardd]: input:
sample 1 type 1 finger ...` (type 1 = down, 2 = move, 3 = up), and the once-a-minute `goodix: so far N reports`
line, where N should grow while the finger is down.

## Status on the phone (2026-11-10, iOS 26.5.2, kernel-16k-drivers-tap.bak)

- The controller probes and polls: `goodix: chip "BERLIN", firmware "9897T" 05.01.80.03, sensor 255`,
  `goodix: goodix touch screen ready; the interrupt line is looked at every 8000 us`, then every minute
  `so far 3 reports in 30000 looks (0 bad)` (idle; nobody touched the glass).
- backboardd takes the service: `dlisa[backboardd]: input: "darwin-lisa touch screen" opened by the event server
  (attempt 1)`, then `touch detection mode 0x1` once the display is on.
- Synthetic tap on the cover sheet's (i) (`touch_synth_tap=2 touch_synth_x=58060 touch_synth_y=62030`): samples 1 (down)
  and 2 (up) reach backboardd and **SpringBoard opens the device information sheet** (SN, Regulatory):
  `research/hackios-xnu-kernel/logs/drv-run3-shots/fullroot-shot-150.png`. Touch works end to end.
- Synthetic swipe up from the home bar: all 22 samples reach backboardd and the cover sheet moves
  (`logs/drv-run2-shots/fullroot-shot-140.png`, `-220.png`), but it springs back instead of opening the phone. The
  touch path is not the cause (the tap works). Most likely the unlock request itself fails (no keybag / the
  keystore stand-in), to be checked with `dlisa_oslog=SpringBoard` around a swipe.
