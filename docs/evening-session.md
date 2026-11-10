# Evening session, 2026-11-10: one flash, four boots

This page merges every run book of the day: `gpu-device-test.md`, `presentation-build.md` §7, the RTC, Wi-Fi join and
ADSP READMEs, and `data-volume.md`. Everything was prepared offline. Nothing here has run on the phone yet.

The plan is one flash (vendor_boot_a plus the root region of super) and then four boots:

| Boot | Kind | What it tests |
|---|---|---|
| 1 | host run (`wifi-run.sh`) | Wi-Fi join (you type the network), the RTC offset kept, ADSP + fuel gauge, the fake rotation path, `lisa=1` |
| 2 | cold standalone boot, no PC | identity and geometry edge to edge, no false hang, the real clock, battery % and charging on a wall charger, Wi-Fi auto-join |
| 3 | host run (`gpu-run.sh probe`) | the GPU up with built-in firmware, the probe's triangles, Metal's device |
| 4 | host run (`gpu-run.sh ca`) | CoreAnimation on Metal |

The data region of super (the persistent `/private/var`) is **never written by hand** in this session. Only the
kernel writes to it while iOS runs.

## The builds

| What | File | Size | sha256 |
|---|---|---|---|
| kernel | `research/hackios-xnu-kernel/kernel-16k-final1.bak` (= `kernel26.nopac` now) | 45,521,816 | `67b9c3b3c9e714ccfe5121303c883c607541bfad568b1b7239b824266b5ddf0c` |
| root | `userland-26/out/system-26.apfs.full` (copy `system-26.apfs.full-08898bde`) | 4,831,838,208 | `08898bde7b97221965b6c4665db894458a3d08e5c002e4f2db53c4a464598f42` |
| vendor_boot | `out/vendor_boot-lisa-combined.img` (copy `out/vendor_boot-lisa-final1-390aee3e.img`) | 45,817,856 | `390aee3e48ff82f770569e5add37890d285e701404f6f705c4af5a5855d9fce2` |
| its HKUB | `out/loader-usb-lisa-combined.hkub` | 45,608,960 | `8eeec77247c31d29f9e93c6432a80926b87bbf71d9e5f4c403adecdb3cc2a7a0` |
| its loader | `out/loader-combined.efi` | | `220880a7a7a867a0a512c5532cef29d0d0912dfaf3b24c9d4c95e93eb16bec30` |
| its payload | `out/combined-payload.bin` | 45,522,376 | `879426dd39e440869ed8681d107fbd4e972d0421173f6aa1a2112029bbf3e75f` |

The kernel is present2 plus everything since:

- GPU: HakiosGPU with built-in GMU/SQE/zap firmware, and `metal_plugin=`.
- RTC: `hakios_rtc`, PMK8350 over SPMI.
- Wi-Fi join: the iOS 26 request shapes, the flash root's `fw/`, and `kern.hakios.wifi_network`.
- ADSP: PIL, GLINK, QRTR, battmgr, the SSC client, and the **built-in ADSP firmware**.
- Battery: the fuel-gauge sink.
- `lisa=1` boot-arg defaults.

Every marker string was checked in the binary:

- `segments after it: built into the kernel`, `THE AUDIO DSP IS UP AND LINKED`, `ssc: the sensor core`, `battery_source`
- `the PMK8350's clock`, `THE NETWORK OF THE FILE IS JOINED`, `wifi-net:`, `image_ufs`
- `touch_avdd_ldo=ldoc7 touch_chip=goodix` (the defaults)
- `bytes built into the kernel`, `HakiosGPUUserClient`, `THE GPU STAYS ON`, `touch screen ready`
- `take writes as /dev/ufs_data`

`adsp.mdt`, `adsp.b29` and `adsp.b36` were found byte for byte, each on a 4 KB boundary.

The root is dc2a9f49 (D16 profile + GPU userland) plus:

- init26 with the RTC keeper and the Wi-Fi handoff, with wifid gated on `wifi=1`;
- wifid's plist without `_LimitLoadToDeviceTree`;
- `libdlisa-motion.dylib` injected into backboardd;
- libdlisa rebuilt (same bytes).

1,485 blocks (5.8 MB) are still free under the budget.

The vendor_boot holds the loader (run-tpl, live-watch, identity-soc), the iPhone 13 mini identity (t8110, 0x8110, board
0x08, A2481) and this kernel. Its autoboot line is 351 characters (`out/combined-cmdline.txt`):

```
rd=md0 ufs=2 ufs_part=super ufs_root=1 ufs_write=1 ufs_data=super var_ufs=1 dlisa_unlock=1 dlisa_noidle=1
dlisa_watchdog=4 display=1 keystore=1 console_open=1 lisa=1 cpus=7 battery=1 wifi=1
init_run=/usr/libexec/usermanagerd:--init init_run_secs=60 bootcpu=7 loader_return_after=86400 epoch=1794336680
fbwidth=1080 fbheight=2400 rtc=1 adsp=1 battmgr=2
```

(`epoch=1794336680` is 2026-11-10 14:51 local time. It is only the fallback if the RTC gives nothing.)

## The decisions

**Root size budget.** The ADSP firmware (20.8 MB, 10.5 MB once compressed by mkapfs) is **built into the kernel**
(option a). It does not go in the root.

- `adsp-wire.py` step 5 copies the staged set into `bsd/dev/hakios.adsp_fw/` and generates `hakios.adsp_fw.h`.
  Both are in the tree only, never in git. The header assembles the files into `__TEXT,__const` with `.incbin`.
- `hakios.adsp.c` `image_file()` asks the RAM disks and the flash root's `fw/` first, then the built-in set.
- The kernel grows from 23.4 MB to 45.5 MB, under the loader's 63 MiB payload. Its extent is 95.7 MB above the
  virtual base, under the 128 MB where a resident RAM disk goes.
- `install-root.sh` no longer copies the firmware (`ADSP_FW_ROOT=1` would) and removes any `adsp.*` left in `fw/`.
- Bonus: `wifi-run.sh` copies the root's `fw/` into a 32 MiB RAM disk. With 20 MB of ADSP firmware there it would have
  overflowed.
- Option (c), moving the data region, was not needed. The data volume and its state stay where they are.

**Boot-arg line budget.** There is a new kernel mechanism, `lisa=1` (`kernel/drivers/bootargs/lisa-defaults-wire.py`,
`pexpert/gen/bootargs.c`).

- When a name is not on the line and the line has `lisa=1`, `PE_parse_boot_argn`/`PE_parse_boot_arg_str` take it
  from the built-in list.
- The list is `touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix touch_goodix_a=1 cpufreq=1 cpufreq_max=2400
  fbwidth=1080 fbheight=2400 wifi_irq=0 wifi_mac=02:6c:69:73:61:01`.
- The line always wins over the list.
- Without `lisa=1` nothing changes. That covers `run-flashroot.sh`, `gpu-run.sh` and `wifi-run.sh` as they were, and
  every old explicit argument is still accepted.
- What the loader reads stays on the line: `cpus=`, `bootcpu=`, `battery=1`, `ufs=`, `ufs_data=`, `rd=`,
  `loader_return_after=`.
- What userland reads from `kern.bootargs` stays on the line too: `console_open=1`, `init_run*`, `wifi=1`, `dlisa_*`.
- `sb_delay=8` was dropped. Only HakiOS 16.5's libhakios.sb read it; nothing in the 26 kernel or root does.
- `wlan_stub=1` gives way to `wifi=1`.
- Host test: `sh kernel/drivers/bootargs/test/run-tests.sh`. It wires the pristine `bootargs.c` twice, compiles it and
  checks 30 cases.
- The line went from 434 characters (no RTC, ADSP or Wi-Fi) to 351 (with them).
- `build-standalone.sh` refuses `LISA_DEFAULTS=1` with a kernel that lacks the list.

**Autoboot defaults.**

- On: RTC (`rtc=1`), ADSP with the fuel gauge (`adsp=1 battmgr=2`), and Wi-Fi (`wifi=1`). The radio joins the network
  kept on the data volume, if one is kept, and init26 lets wifid run.
- Off: the GPU, the sensors (`sensors`, `sensors_fake`), and `wifi_loud`.
- To change them, use `FEATURES=`, `WIFI=0` or `LISA_DEFAULTS=0` with `loader/build-combined.sh`.

**Wire order** (applied in docker as root, then the rebuild; log `darwin-lisa/logs/final1-wires.log`):

1. battery
2. adsp
3. rtc
4. wifi-join
5. bootargs
6. gpu (last)

The second run of all six printed only `[skip]`. Three wires used to print `[ok]` on every run, so this needed fixes:

- `battery-wire.py` failed on a tree where a later tag sits after `hakios_battery`. It now checks for the tag and
  honours `XNU_WORK=`.
- The copy steps of `dlisa_wire.py`, `gpu-wire.py` and `battery-wire.py` now skip identical files.

`kernel.log` has one `error:` hit, the known IOLog string `KernelConfigTables syntax error` in `IOCatalogue.cpp`.

## Before you start (PC)

```sh
cd ~/Dev/mobile
sha256sum research/hackios-xnu-kernel/kernel-16k-final1.bak out/vendor_boot-lisa-combined.img   # 67b9c3b3… 390aee3e…
cut -c1-64 userland-26/out/system-26.apfs.full.sha256                                         # 08898bde…
read -rs -p "pmOS password: " PMOS_PASS && export PMOS_PASS && export SSHPASS=$PMOS_PASS   # typed, never in a file
# pmOS as mateus, and as root (sudo reads the password from stdin; the command must not contain a single quote)
pm() { sshpass -e ssh -o ConnectTimeout=5 mateus@172.16.42.1 "$@"; }
pmroot() { printf '%s\n' "$PMOS_PASS" | sshpass -e ssh -o ConnectTimeout=5 mateus@172.16.42.1 "sudo -S -p '' sh -c '$1'"; }
```

Charging rule: iOS used not to charge. With `adsp=1` it may charge now; Boot 2 checks that. Until it is proven:

- park the phone in pmOS (`darwin-lisa/tools/bootos.sh pmos`, which keeps it awake) between boots whenever the last
  VBAT/capacity line is low (below about 3.8 V or 40%);
- always park it at the end.

`bootos.sh pmos` reboots a loader that has run a kernel by itself.

## Step 0: pmOS, read only

```sh
darwin-lisa/tools/bootos.sh state
darwin-lisa/tools/bootos.sh pmos

# 0.1 super is sda20, and what is on the phone now (expected root f2c62ac1…, vendor_boot abfe7629… = D22 standalone)
pm 'readlink -f /dev/disk/by-partlabel/super; cat /sys/class/block/sda20/start /sys/class/block/sda20/size;
    grep PARTNAME /sys/class/block/sda20/uevent; df -h /home/mateus'
pmroot "head -c 24698880 /dev/disk/by-partlabel/vendor_boot_a | sha256sum; head -c 4831838208 /dev/disk/by-partlabel/super | sha256sum"
```

Expect `/dev/sda20`, `4587520`, `17825792` and `PARTNAME=super`. `/home/mateus` needs at least 5.5 GB free. If the
partition facts differ, **stop**.

```sh
# 0.2 the battery as pmOS sees it (compare with Boot 1's battmgr lines; read within a minute of rebooting)
pm 'cd /sys/class/power_supply && for f in capacity voltage_now current_now temp status health cycle_count \
    charge_full charge_full_design; do printf "%s=%s " $f "$(cat qcom-battmgr-bat/$f)"; done; echo;
    echo "usb online=$(cat qcom-battmgr-usb/online) type=$(cat qcom-battmgr-usb/usb_type)"' | tee darwin-lisa/logs/evening-pmos-battery.txt

# 0.3 the RTC offset (PMK8350 counter vs this PC's UTC; keep the PC on NTP)
RTC=$(darwin-lisa/tools/rtc-offset.sh) && echo "$RTC" | tee darwin-lisa/logs/evening-rtc-offset.txt   # rtc_offset=N
```

## Step 1: the flash [WRITES: vendor_boot_a, super bytes 0..4831838207]

```sh
# 1.1 backups and a hash of the data region (read only)
pmroot "dd if=/dev/disk/by-partlabel/vendor_boot_a of=/home/mateus/vendor_boot_a-before-final1.img bs=1M &&
    dd if=/dev/disk/by-partlabel/super bs=1M skip=4608 count=4096 2>/dev/null | gzip -1 > /home/mateus/var-before-final1.gz"
pm 'zcat /home/mateus/var-before-final1.gz | sha256sum' | tee darwin-lisa/logs/evening-var-before.txt
sshpass -e scp mateus@172.16.42.1:/home/mateus/vendor_boot_a-before-final1.img mateus@172.16.42.1:/home/mateus/var-before-final1.gz backups/

# 1.2 the images onto the phone, checked there before any write
sshpass -e scp out/vendor_boot-lisa-combined.img userland-26/out/system-26.apfs.full mateus@172.16.42.1:/home/mateus/
pm 'sha256sum /home/mateus/vendor_boot-lisa-combined.img /home/mateus/system-26.apfs.full'     # 390aee3e…, 08898bde…

# 1.3 vendor_boot_a
pmroot "dd if=/home/mateus/vendor_boot-lisa-combined.img of=/dev/disk/by-partlabel/vendor_boot_a bs=1M conv=fsync &&
    echo 3 > /proc/sys/vm/drop_caches && head -c 45817856 /dev/disk/by-partlabel/vendor_boot_a | sha256sum"   # 390aee3e…

# 1.4 the root: 4608 MiB exactly, never past it
pmroot "dd if=/home/mateus/system-26.apfs.full of=/dev/disk/by-partlabel/super bs=1M count=4608 conv=notrunc,fsync &&
    echo 3 > /proc/sys/vm/drop_caches && head -c 4831838208 /dev/disk/by-partlabel/super | sha256sum &&
    dd if=/dev/disk/by-partlabel/super bs=1M skip=4608 count=4096 2>/dev/null | sha256sum"
```

Pass:

- 1.3 reads back `390aee3e…`.
- 1.4 reads back `08898bde…` for the root.
- 1.4's second hash (the data region) equals 1.1's.

If a read-back differs, write that step again. Do not boot.

Then `pm 'rm /home/mateus/vendor_boot-lisa-combined.img'`. Keep `system-26.apfs.full` on the phone until the end, so a
rewrite needs no copy. Then `darwin-lisa/tools/bootos.sh hakios`.

## Boot 1: host run: Wi-Fi join, RTC, ADSP/battmgr, sensors (about 25 min)

This boot is the first run of the new kernel, with the full log. It also leaves what Boot 2 needs on the data
volume: the network (`/private/var/db/dlisa-wifi/network`, 0600) and the RTC offset (`/private/var/db/dlisa-rtc-offset`).
Use a WPA2 or WPA2/WPA3-transition network: WPA3-SAE alone is not implemented.

```sh
cd ~/Dev/mobile
RTC=$(cat darwin-lisa/logs/evening-rtc-offset.txt)
KERNEL=kernel-16k-final1.bak XL_SECS=1500 XL_SHOT_EVERY=20 XL_TAG=evening-b1 \
WIFI_ARGS="wifi=1 wifi_loud=1 rtc=1 $RTC" \
XL_EXTRA_ARGS="display=1 keystore=1 console_open=1 lisa=1 cpus=7 battery=1 dlisa_unlock=1 dlisa_noidle=1 dlisa_watchdog=4 adsp=1 battmgr=2 sensors=1 sensors_fake=2 init_run=/usr/libexec/usermanagerd:--init init_run_secs=60" \
darwin-lisa/tools/wifi-run.sh             # type the network's name and passphrase at the prompts
L=research/hackios-xnu-kernel/logs/evening-b1.log
```

The line is about 420 of 460 characters with a 10-digit offset. The phone must be in the loader: the script starts
with `fastboot reboot`. When the home screen shows (in the shots, about 5 min), **open Safari or Calculator by hand**
and leave it open. The fake rotation starts about 2.5 min after the DSP links. Nothing else needs a hand.

Checks, in this order:

```sh
grep -a 'TRUNCATED' $L                                        # nothing
grep -a -E 'touch screen ready|MiniIOMobileFramebuffer: display' $L | head -3     # lisa=1 gave touch and 1080x2400
# ADSP
grep -a -E 'adsp: (the audio DSP is being|[0-9]+: fw/adsp.mdt and)|IS OPEN both|HELLO FROM NODE|THE AUDIO DSP (IS UP|DID NOT)|LAST WORDS' $L
grep -a -E 'adsp: battmgr: (the DSP says|NO ANSWER|full charge|battery property|USB property)' $L | head -60
grep -a -E "HakiosBattery: the battery's power source" $L
# RTC
grep -a -E "^rtc: |init26: RTC" $L
# Wi-Fi
grep -a -E 'wifi: THE (RADIO IS UP|NETWORK OF THE FILE IS JOINED)|init26: WIFI|wifi-net:|apple80211: in this run' $L
grep -a -E 'IPConfiguration|DHCP|en0.*(lease|inet)' $L | head
# sensors and the rotation path
grep -a -E '^ssc: (the sensor core|NO SENSOR|sensors_fake|events:)|dlisa-motion: ' $L | head -40
```

Pass:

1. **ADSP:** `fw/adsp.mdt and the 35 segments after it: built into the kernel`, both channels `IPCRTR` and
   `PMIC_RTR_ADSP_APPS` OPEN, `HELLO FROM NODE`, then `THE AUDIO DSP IS UP AND LINKED` within 60 s of the root mount.
   No panic, and Wi-Fi still comes up. A failure names the step (`DID NOT COME UP: step N`); write it down with
   `LAST WORDS`.
2. **Fuel gauge:** `battmgr: the DSP says: capacity C` within 2 points of pmOS's `capacity` (C is hundredths), the
   same cycles, temperature within 3 °C. Check the numbering against the property dump first: property 4 is capacity,
   7 voltage, 9 current, 13 temperature, 16 cycles. If lisa puts temperature at 12, `BATT_TEMP` and the four after it
   move. Also `HakiosBattery: the battery's power source is up: N%`. The status bar shows the same %. Current sign:
   if the firmware reports positive while charging, add `battmgr_sign=1` to the next lines.
3. **RTC:**
   - `rtc: the PMK8350's clock: channel …, control 0x80 (running), count C s`;
   - `rtc: offset O s from the boot-arg …: the time is …`, within about 2 s of the PC's UTC at that moment;
   - `init26: RTC the kernel's offset is saved when it changes`;
   - about 30 s later, `init26: RTC offset O s saved for the next boot`.

   A stopped counter (`the counter does not run`) means a later boot needs `rtc_write=1`. Not needed if it runs.
4. **Wi-Fi:**
   - `wifi: THE RADIO IS UP`, then `wifi: THE NETWORK OF THE FILE IS JOINED`;
   - `init26: WIFI the network of /dev/md0's fw/network is kept on the data volume (N bytes)`;
   - `apple80211: in this run: …`, the request summary. Write down every request that was not answered;
   - then DHCP (IPConfiguration). An IP is the goal, but no IP while associated is a known risk: wifid must create the
     en0 service.
5. **Sensors:**
   - expected: `ssc: NO SENSOR CORE` or a sensor list without accelerometer events, then
     `ssc: sensors_fake=2: no accelerometer event after N s: made-up values from now on`;
   - `dlisa-motion: … replaced`, `dlisa-motion: the device orientation comes from the kernel's sensors`, then
     `dlisa-motion: the device is now …` every 15 s;
   - the open app turns in the shots (`research/hackios-xnu-kernel/logs/evening-b1-shot-*.png`).

   Real accelerometer events (`ssc: events: accel N` with N growing, about 9800 mm/s² on one axis) would be a bonus.
   The README explains why they are unlikely (no FastRPC file server).

When the run ends the phone is back in the loader. If the battery is low, park it: `darwin-lisa/tools/bootos.sh pmos`.

## Boot 2: the cold standalone boot, no PC (about 20 min)

This is the presentation path, so do it exactly like this:

1. Unplug the cable. Hold Power until the phone is off. Press Power.
2. Expect the ABL logo, the U-Boot menu (5 s), the loader's `autoboot: iOS starts in 10 s`, then iOS. Do not connect
   anything during the first 10 s: any fastboot command cancels the autoboot.
3. Leave it alone for **at least 3 minutes**, with no cable and no polling. The old false hang came at about 74 s.
4. On the phone:
   - the lock screen clock shows **the real local time** (compare with the PC);
   - the status bar battery % is near Boot 1's capacity;
   - the Wi-Fi icon appears once the network is joined (allow a minute or two);
   - swipe up to the home screen, open Settings and scroll. Optionally check General > About (iPhone 13 mini) and the
     Wi-Fi row (the network's name).
5. Plug in the PC cable and collect log 1 and a screenshot:

   ```sh
   cd ~/Dev/mobile/research/hackios-xnu-kernel
   fastboot oem log && fastboot get_staged logs/evening-b2-a.log
   fastboot getvar screen                                     # 1080 2400 <stride>
   fastboot oem screenshot && fastboot get_staged /tmp/b2.raw &&
     python3 ~/Dev/mobile/darwin-lisa/tools/fbraw2png.py /tmp/b2.raw logs/evening-b2-shot.png 1080 2400 <stride>
   ```

6. Unplug the PC and plug in a **wall charger** (DCP/QC/PD, not a PC port). Wait 10 minutes. The lock screen should say
   "Charging" and the status bar should show the bolt.
7. Unplug the charger. Within a second the bolt goes. Plug in the PC cable and collect log 2:
   `fastboot oem log && fastboot get_staged logs/evening-b2-b.log`.

Checks:

```sh
B=logs/evening-b2-b.log; A=logs/evening-b2-a.log
grep -a -E 'THE KERNEL HUNG|no sign of life|alive again|taken for hung' $A $B      # expected: nothing
grep -a 'MiniIOMobileFramebuffer: display' $A | head -2                              # 1080x2400 on a panel of 1080x2400
grep -a -E "^rtc: |init26: RTC" $A                                                   # the saved offset is the kernel's now
grep -a -E 'init26: WIFI|wifi-net:|wifi: THE' $A
grep -a -E 'adsp: THE AUDIO DSP|adsp: battmgr: (the DSP says|< a notice)|reported as charging' $B | tail -30
```

Pass:

1. **No false hang.** None of the four strings appears. A `no sign of life … still watching` followed by
   `alive again` means the old trap was sprung and survived: write down the times.
2. **Edge to edge.** The screenshot has UI or wallpaper on all four edges, no black bars. The status bar is split
   (time left, battery right, empty around the punch-hole). Circles are round. The loader's text over the screen is
   expected.
3. **Clock.** `init26: RTC the saved offset O s is the kernel's now`, and the lock screen time matches the PC to the
   minute.
4. **Battery.** `battmgr: the DSP says` lines with the capacity of the status bar.
5. **Charging**, during the 10 minutes on the wall charger:
   - `USB online 1`, type 2 or more;
   - status 1 (or the `reported as charging` line);
   - a reported current above +300 mA;
   - capacity and µV rising;
   - "Charging" and the bolt on screen;
   - on unplug, `< a notice`, then `USB online 0`.
6. **Wi-Fi** (only if Boot 1 kept a network):
   - `init26: WIFI the kept network (N bytes) is the kernel's to join`;
   - `wifi-net: the kept network … asked for … s after it was given: …` (or `… is the one the radio is on already`);
   - the Wi-Fi icon.

**If the RTC did not work in Boot 1 or 2,** rebuild the vendor_boot right away so that its `epoch=` is fresh, then
flash only vendor_boot_a (step 1.3 with the new size and sha):

```sh
FEATURES="adsp=1 battmgr=2" darwin-lisa/loader/build-combined.sh   # rtc=1 out if it misbehaves; plain build-combined.sh if it only gave no time
```

## Boot 3: GPU probe (host run, about 15 min)

Only the boot-args differ from a normal boot: the GPU firmware is built in, and the userland is in the root. Pass
criteria and every fallback are in `gpu-device-test.md` "Boot 1".

```sh
cd ~/Dev/mobile
GEOM="fbwidth=1080 fbheight=2400" KERNEL=kernel-16k-final1.bak darwin-lisa/kernel/drivers/gpu/gpu-run.sh print probe   # check the line
GEOM="fbwidth=1080 fbheight=2400" KERNEL=kernel-16k-final1.bak darwin-lisa/kernel/drivers/gpu/gpu-run.sh run probe
L=$(ls -t research/hackios-xnu-kernel/logs/gpu-probe-*.log | grep -v -- -at- | head -1)
grep -a 'adreno:' $L | grep -E 'built into the kernel|step [0-9]+: (done|FAILED)|IS ALIVE|FIRMWARE IS LOADED|ZAP SHADER|TAKES COMMANDS|0x600dc0de|STAYS ON|NOT STARTED'
grep -a -E 'init26: RUN .*gputest|gputest:|HakiosGPU: submission|probe: (ok|FAIL)|probe: [0-9]+ failure' $L | head -80
grep -a -E 'MiniIOMobileFramebuffer: metal=1|metal_test:|HakiosMetal\[' $L | head -60
```

Pass:

- **Step 1:** the four `fw/a660_*: N bytes built into the kernel` lines, then steps through 21 `done`, the zap shader
  started, `0x600dc0de`, and `THE GPU STAYS ON`.
- **Step 2:** `probe: 0 failures` and `gputest: … exit status 0`.
- **Step 3a:** `the accelerator names HakiosMetalProbe`, `MTLCreateSystemDefaultDevice() = …`, `blits WORK`.
- The boot goes on to the home screen in software.
- If the zap is refused, run boot 3 again with `XL_FW_RAMDISK=userland-26/out/fw-gpu-pmos-pad32.apfs`.

## Boot 4: CoreAnimation on Metal (host run, about 20 min)

Only after Boot 3 passed steps 1 and 2.

```sh
GEOM="fbwidth=1080 fbheight=2400" KERNEL=kernel-16k-final1.bak darwin-lisa/kernel/drivers/gpu/gpu-run.sh run ca
L=$(ls -t research/hackios-xnu-kernel/logs/gpu-ca-*.log | grep -v -- -at- | head -1)
grep -a -E 'HakiosMetal\[backboardd|function constants:|HakiosGPU: [0-9]+ submissions|fps:|MiniIOMobileFramebuffer: [0-9]+ frames' $L | head -60
grep -a -E 'backboardd.*(crash|exited|abort)|DLISA abort.*backboardd|metallib_url' $L | head
```

Pass, in order:

1. The plug-in loads in backboardd.
2. `function constants: … keep their descriptor's values`.
3. `HakiosGPU: N submissions` and a non-zero `waiting for the GPU`.
4. `fps:` lines.
5. No backboardd restarts and no `metallib_url`.
6. A correct lock or home screen in the shots.

Contingencies: `gpu-run.sh run ca-lite` (blur slow or refused) and `run ca-native` (shapes drawn wrong). See
`gpu-device-test.md`.

**End of session:** `darwin-lisa/tools/bootos.sh pmos` (it charges and stays awake). Then
`pm 'rm /home/mateus/system-26.apfs.full'` once nothing needs a rewrite.

## If something goes wrong

| Symptom | Do |
|---|---|
| Boot 1: `THE AUDIO DSP DID NOT COME UP` or a panic in adsp/battmgr | Boot 1 again without `adsp=1 battmgr=2 sensors=1 sensors_fake=2` to finish Wi-Fi and RTC. Before Boot 2, rebuild with `FEATURES="rtc=1" darwin-lisa/loader/build-combined.sh` and flash vendor_boot_a only (1.3) |
| Boot 1: Wi-Fi panics or stalls the boot | rebuild with `WIFI=0` (wlan_stub=1) for the autoboot |
| Boot 1: no touch, or the old 1125x2436 size | `lisa=1` did not act. Host runs: put `touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix touch_goodix_a=1 cpufreq=1 cpufreq_max=2400 wifi_irq=0 wifi_mac=02:6c:69:73:61:01` in place of `lisa=1` (drop `dlisa_watchdog=4 wifi_loud=1` if the line is too long). Autoboot: `LISA_DEFAULTS=0 WIFI=0 darwin-lisa/loader/build-combined.sh` (443 characters; with Wi-Fi too it would be 479, over the 460 the script allows) |
| Boot 1: associated but no IP | known risk (wifid has to create the en0 service). Note the `apple80211: in this run:` summary; the static-service fallback is offline work |
| Boot 1: `the line is TRUNCATED` | drop `dlisa_watchdog=4` or `wifi_loud=1` |
| Boot 2: the loop menu, then iOS, over and over | the U-Boot menu is shown 5 s each time: pick postmarketOS with the volume keys, then roll back vendor_boot_a |
| Boot 2: U-Boot says an HKUB entry is damaged, or waits for the host | the vendor ramdisk (45.6 MB, twice the old one) was not intact: Power + menu gives pmOS, then roll back vendor_boot_a, and report it |
| Boot 2: 1080x2400 layout broken at 360 points | in Boot 3 use `GEOM="fbwidth=1125 fbheight=2499"`; if that is right, the next build is `GEOMETRY=1125x2499 darwin-lisa/loader/build-combined.sh` (then also add `fbwidth=1125 fbheight=2499` explicitly: the line wins over `lisa=1`) |
| a data volume problem (`HFS Plus from /dev/ufs_data: <errno>`) | see `data-volume.md` "If something goes wrong". `backups/var-before-final1.gz` is this morning's region as it was before the session |
| forget the Wi-Fi network | one boot with `wifi_forget=1` |

## Rollback

| What | How |
|---|---|
| vendor_boot_a | `backups/vendor_boot_a-before-final1.img` (step 1.1), or `out/vendor_boot-lisa-standalone-d22-abfe7629.img`, the image that was on the phone. Use pmOS `dd` as in 1.3, or `fastboot flash vendor_boot_a` from ABL (`darwin-lisa/tools/abl-fastboot.sh`, or Power + Volume Down). boot_a (U-Boot) is not touched, so pmOS is always one volume key away in the menu |
| root | `userland-26/out/system-26.apfs.full-f2c62ac1` (the root that was on the phone), through 1.2 and 1.4 with its sha. Others: `-prev-20261110-dc2a9f49` (the GPU candidate, never flashed), `-199a4181` |
| kernel (host runs) | `KERNEL=kernel-16k-gpu1.bak` (de12f63f: GPU, no RTC/ADSP/Wi-Fi join/`lisa=1`) or `kernel-16k-present2.bak` (c54833dc). A host run picks its kernel per run, with no flash |
| one autoboot feature | `FEATURES=…`, `WIFI=0`, `LISA_DEFAULTS=0` with `darwin-lisa/loader/build-combined.sh`, then 1.3 only |
| the data volume | `backups/var-before-final1.gz` (it holds no Wi-Fi credential: it was taken before Boot 1). Write it back with `var-write.sh write` after `gzip -d` (its sha is in `logs/evening-var-before.txt`). A region backup taken AFTER Boot 1 contains the network's passphrase: keep it out of git and out of `backups/` |
| the kernel tree | `darwin-lisa/.work/xnu-work-before-final1.tgz` (config, conf, bsd/dev, bsd/sys, iokit/Kernel, pexpert/gen before the wires) |
| the sysroot | `darwin-lisa/.work/sysroot-before-final1/` (launchd, libdlisa.dylib, dlisa-inject.conf, com.apple.wifid.plist before), and remove `usr/local/lib/libdlisa-motion.dylib` |

## How these were built (to redo any piece)

```sh
cd ~/Dev/mobile
# kernel: the wires in order, in docker as root (each one twice: the second must be all [skip]), then the rebuild
docker run --rm -v ~/Dev/mobile:/home/mateusseiboth/Dev/mobile darwin-lisa/unixnu-toolchain:latest sh -c \
  'cd /home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/drivers && for w in battery/battery-wire.py adsp/adsp-wire.py \
   rtc/rtc-wire.py wifi-join/wifi-join-wire.py bootargs/lisa-defaults-wire.py gpu/gpu-wire.py; do python3 -I $w; done'
sh darwin-lisa/.work/run-rebuild.sh > darwin-lisa/logs/rebuild-final1.log 2>&1
grep -n 'error:' research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/logs/kernel.log   # one hit: the IOCatalogue IOLog string
cp -n research/hackios-xnu-kernel/kernel26.nopac research/hackios-xnu-kernel/kernel-16k-final1.bak
# root (order matters)
sh darwin-lisa/userland/inject/build-inject.sh
python3 -I darwin-lisa/userland/scripts/rtc-wifi-userland-wire.py
#   mirror to userland-26: init/{init-26.c,rtc-offset.c,wifi-net.c,rtc-wifi-boot.c} scripts/{plist-tweaks.py,lean-daemons.txt}
bash userland-26/scripts/build-root-26.sh init-only && install -m 0755 userland-26/out/launchd userland-26/out/sysroot-26/sbin/launchd
sh darwin-lisa/kernel/drivers/adsp/install-root.sh userland-26/out/sysroot-26
(cd userland-26 && FULL=1 bash scripts/mkimage-trim.sh)        # must say 4831838208 bytes
# vendor_boot (defaults: kernel-16k-final1.bak, lisa=1, wifi=1, rtc=1 adsp=1 battmgr=2, D16, 1080x2400)
darwin-lisa/loader/build-combined.sh
```

Host tests that pass:

- `sh darwin-lisa/kernel/drivers/bootargs/test/run-tests.sh`
- `sh darwin-lisa/kernel/drivers/adsp/test/fw-builtin-test.sh`
- `sh darwin-lisa/kernel/drivers/adsp/test/run-tests.sh`
- `sh darwin-lisa/userland/init/test/run-tests.sh`
- `python3 -I darwin-lisa/userland/scripts/test_plist_tweaks.py`
