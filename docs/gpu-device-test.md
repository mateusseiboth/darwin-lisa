# GPU on the phone: the run book (one session)

## Session 3 (night of 2026-11-10): CoreAnimation draws the UI on the GPU

**Result.** Host run `gpu-q1` (logs `research/hackios-xnu-kernel/logs/gpu-q1-*`):
- the home screen is drawn by CoreAnimation on the Adreno;
- a synthetic tap on the Settings icon (`touch_synth=200 touch_synth_tap=2 touch_synth_x=56675 touch_synth_y=4751`)
  opened Settings, also drawn correctly;
- no GPU hang;
- the GPU was switched off at the end.

**What was wrong, and the fixes:**
- **The uber shader.** `fixed_frag_lph_cph`/`_cpf` is 38 560 instruction words because a 148-byte private array lives
  in scratch memory. `hakios-metal-spec-s2v.patch` turns scratch at constant offsets back into values
  (`nir_lower_scratch_to_var`) and cleans up. All 43 constant sets the window server asked for compile to 32-768 words,
  and the full library sweep still compiles 37/37 vertex and 128/128 fragment functions. One draw used to take seconds
  and could stall the GPU ("no answer").
- **`HakiosMetalCA-spec-bound.patch`.** With `metal_spec_wait=1`, the whole uber shader never runs:
  - a program that is still at 4096 words or more is specialised on the buffer the draw binds;
  - the draw waits for that compile, because a dropped draw is never redrawn;
  - a draw whose specialised program is refused is left out.

  It is a safety net now; the window server's programs are below the threshold.
- **The way out of a run.** The 16 KB-page kernel never returns to the loader (`loader_jump` exists only for 4 KB
  pages), and nothing gave the GPU back, so every run ended in a warm reset with the GPU running.
  `kernel/loader-console/loader_console.c` now handles `oem stop` and `loader_return_after` in order:
  - it calls `hakios_adreno_stop()` (core finished or halted, GMU reset, zap shader given up, GX and CX off);
  - it logs `--- the run is over: ... reset the phone now (fastboot reboot) ---`;
  - `loader_return()` quiesces once;
  - `hakios_gpu_submit()` refuses after the stop.

  Kernel: `kernel-16k-final1-fix-gpustop2.bak` (25d0ce05).

**Running a GPU test now:**
- End every run with `fastboot oem stop` and wait for "the run is over" before any reset.
- Screenshots taken while the host polls are wrong: the loader's console scrolls the whole panel. Take screenshots
  in a quiet run instead: no polling, spaced screenshots.

## Session 2 (after `gpu-probe-1551`): step 3a fixed, re-run 3a then 3b

**What failed.** Steps 1 and 2 passed on kernel-16k-final1. Step 3a aborted in metal_test:
`MTLCreateSystemDefaultDevice doesn't know what to do with more than one device`. Metal 26's
`MTLCreateSystemDefaultDevice` block (`sub_1860c7158` in 23F84) takes the device array and asserts unless its `count`
is exactly 1. There were two devices because there were two `IOAcceleratorES "sgx"` services: metal_test listed both,
and the kernel log has `hakios display: panel …` and `the accelerator names HakiosMetalProbe` twice. **Every iOS 26 kernel
since the display port started the display twice** (the same doubled line is in `cpu-b3-smp7-vnodes.log` and in the
evening logs): `bsd_init.c` called `hakios_display_init()` from two blocks. A rerun of `display-wire.py` added its block
again once `diag-iotrace-wire.py`/`keystore-wire.py` had grown the first one, because its "already applied" text no
longer matched. So there were two IOSurface roots, two framebuffers and two accelerators.

**The fix, three layers:**
- `display/display-wire.py`: one call only. A second bare block that is already there is removed, and the script now
  honours `XNU_WORK=`.
- `display/MiniIOMobileFramebuffer.cpp`: `hakios_display_init()` runs once; a second call logs
  `hakios display: already started; this second call does nothing`.
- Both plug-ins make **one device per process**. A second accelerator service gets nil, which Metal skips: the CA one
  through `HakiosMetalCA-ios26.patch`, the probe one through `probe-plugin-onedevice.patch`. They log
  `accelerator port N: a device was made already, and Metal wants one`. This layer alone fixes step 3a on the final1
  kernel too.

**Builds:**

| What | sha256 | |
|---|---|---|
| kernel `kernel-16k-final2.bak` | see the report | final1 + `adsp/sensors-wire.py` + `power/nosleep-wire.py` + display dedupe + gpu (last). Wires in `logs/final2-wires.log`, build in `logs/rebuild-final2.log` |
| root `system-26.apfs.full` | see the report | the plug-ins with the one-device guard. HakiosMetal.bundle `26e52907`, HakiosMetalProbe.bundle `36c627cf` |

**Writes:**
- The root region (`super`, 4,831,838,208 B) as in step 0.2 below.
- vendor_boot_a (the kernel changed): `KERNEL=research/hackios-xnu-kernel/kernel-16k-final2.bak darwin-lisa/loader/build-combined.sh`,
  then the same flash and read-back as before.

**Boots** (host runs, phone in the loader):

```sh
cd ~/Dev/mobile
export KERNEL=kernel-16k-final2.bak
GEOM="lisa=1" MORE="noidle=1 dlisa_noidle=1" darwin-lisa/kernel/drivers/gpu/gpu-run.sh run probe   # 3a again (1 and 2 too)
GEOM="lisa=1" MORE="noidle=1 dlisa_noidle=1" darwin-lisa/kernel/drivers/gpu/gpu-run.sh run ca      # Boot 4: 3b
```

With `lisa=1` the script leaves the touch and cpufreq arguments to the kernel's built-in defaults. The lines are 402
(probe) and 388 (ca) characters. The touch group now has `touch_chip=goodix` too, for lines without `lisa=1`.

**Pass, re-run 3a:**
- `hakios display: panel …` exactly once, and `the accelerator names HakiosMetalProbe` exactly once.
- metal_test lists ONE `IOAcceleratorES "sgx"`.
- `MTLCreateSystemDefaultDevice() = "…", class HakiosMetalDevice`, `blits WORK`, `gputest: done: … metal_test 0`.
- `probe: 0 failures` again.

Boot 4 passes as in "Boot 2" below.

If the display misbehaves on final2 (one framebuffer instead of two is the only display change), go back to final1:
its plug-in guard still gives exactly one Metal device.

One flash, then at most two host-run boots that differ only in their boot-args. Nothing is uploaded on these boots:
the root and `/private/var` are on the flash, the GPU firmware is built into the kernel, and the GPU userland is in the
root. Every GPU piece is off without its boot-args, so the known-good boot (the vendor_boot's autoboot line) is
unchanged.

Everything here was prepared offline on 2026-11-10. Nothing in it has run on the phone yet.

**Superseded for the flash by `docs/evening-session.md`** (one flash for everything; the final kernel is
`kernel-16k-final1.bak`, 67b9c3b3, and the root 08898bde). The boots below are its Boots 3 and 4.

## The builds

| What | File | sha256 | State |
|---|---|---|---|
| kernel | `research/hackios-xnu-kernel/kernel-16k-gpu1.bak` (= `kernel26.nopac` now) | `de12f63f3317aece…` | present2 (c54833dc) + HakiosGPU + firmware built in + `metal_plugin=`. **Candidate**: the final one is rebuilt once the display/geometry patch for the D16 identity is in (below) |
| root | `userland-26/out/system-26.apfs.full` (copy `system-26.apfs.full-dc2a9f49`) | `dc2a9f49…` | 199a4181 (D16 identity: `D16.Default.plist`, init26 identity-stamp) + GPU userland. 4,831,838,208 B, the budget |
| previous root | `userland-26/out/system-26.apfs.full-199a4181` | `199a4181…` | rollback |
| vendor_boot | built by the loader worker: loader fix + D16 identity + **this** kernel | | not built here |
| pmOS GPU firmware RAM disk | `userland-26/out/fw-gpu-pmos-pad32.apfs` | `50e4ff91…` | contingency only (an upload) |

What the root adds, all inert without boot-args (+712 blocks; the image has 1,490 left before it grows a size step):

| Path | What | Used only with |
|---|---|---|
| `/usr/local/bin/hakios-metal-probe` | GPU test through HakiosGPU: copy, fills, kernels, triangles, checked word by word | `init_run=…/gputest` |
| `/usr/local/bin/gputest` | runs the probe (90 s max) then `metal_test` (45 s max), one line each | `init_run=…/gputest` |
| `/usr/local/bin/metal_test` | Metal's discovery chain, the device, a buffer, blits | (from gputest) |
| `/System/Library/Extensions/HakiosMetal.bundle` | the window server's Metal plug-in (iOS 26 patch, with the AIR->ir3 compiler) | `metal=1` |
| `/System/Library/Extensions/HakiosMetalProbe.bundle` | the discovery plug-in: the device for `metal_test` alone | `metal=1 metal_plugin=HakiosMetalProbe` |
| `/System/Library/Frameworks/QuartzCore.framework/default.metallib` | symlink to the OS cryptex copy, against CA's `metallib_url != nil` assertion | a Metal device |

What the kernel adds, all inert without boot-args:

| Boot-arg | Effect | Without it |
|---|---|---|
| `gpu=1` | the Adreno brought up (21 steps), firmware from a RAM disk if one has it, else built in; service HakiosGPU | the GPU is not touched (as today) |
| `metal=1` | the accelerator names a Metal plug-in | no Metal device anywhere: CoreAnimation draws in software (as today) |
| `metal_plugin=NAME` | which bundle (`HakiosMetal` by default) | |

`dlisa_async=1` (libdlisa) turns CoreAnimation's async rendering back on, for when CA runs on the GPU.

## Before the session (offline): the final kernel

When the display patch for the D16 geometry arrives:

```sh
# the patch into the tree (in docker as root, as the other wires), then:
docker run --rm -v ~/Dev/mobile:/home/mateusseiboth/Dev/mobile darwin-lisa/unixnu-toolchain:latest \
    python3 /home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/drivers/gpu/gpu-wire.py   # idempotent; rerun last
sh darwin-lisa/.work/run-rebuild.sh > darwin-lisa/logs/rebuild-gpu3.log 2>&1
grep -c 'error:' research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/logs/kernel.log   # read any hit: one is an IOLog string
K=research/hackios-xnu-kernel/kernel26.nopac
for s in 'bytes built into the kernel' HakiosGPUUserClient metal_plugin 'THE GPU STAYS ON' 'touch screen ready' ufs_root; do
    printf '%-28s %s\n' "$s" "$(grep -ac "$s" $K)"; done          # every count > 0
cp -n $K research/hackios-xnu-kernel/kernel-16k-gpu.bak && sha256sum research/hackios-xnu-kernel/kernel-16k-gpu.bak
```

That kernel goes to the loader worker for the combined vendor_boot. `gpu-wire.py` reruns safely after a display-wire
copy (it re-guards `MiniIOSurfaceDrawn` and re-adds `metal_plugin=`).

## 0. The flash (once) [WRITES: super, vendor_boot_a]

From pmOS, which `tools/bootos.sh pmos` brings up and keeps awake. The password goes in the environment as
`PMOS_PASS` for that script, never into a file.

1. vendor_boot_a: the loader worker's procedure (docs/presentation-build.md step 5: `dd` with `conv=fsync`, drop caches,
   read back `head -c <size>`, compare the sha256).
2. The root into `super` (docs/flash-boot.md C2, pmOS fallback):

   ```sh
   scp userland-26/out/system-26.apfs.full mateus@172.16.42.1:/home/mateus/
   ssh mateus@172.16.42.1 'sudo dd if=/home/mateus/system-26.apfs.full of=/dev/disk/by-partlabel/super bs=8M conv=fsync &&
     sudo sh -c "echo 3 > /proc/sys/vm/drop_caches" && sudo head -c 4831838208 /dev/disk/by-partlabel/super | sha256sum'
   cut -c1-64 userland-26/out/system-26.apfs.full.sha256      # must be the same: dc2a9f49…
   ```

   The data region after the root is not touched: the image is exactly 4,831,838,208 bytes.

3. `tools/bootos.sh hakios` back to the loader.

## The boots

Both are host runs (`tools/run-flashroot.sh` through `darwin-lisa/kernel/drivers/gpu/gpu-run.sh`): the full kernel
log comes back, root and data volume from the flash, nothing uploaded. `gpu-run.sh print STEP` shows the exact line
first and checks it against the 460-character limit; optional arguments are dropped whole (touch, `dlisa_noidle`,
`metal_defer`) and named when they are. Set `GEOM="<the identity's geometry boot-args>"` for every step.

Things to know:

- The phone must be in the loader. On a cold boot, run the command within the 10 s autoboot window: any fastboot
  command cancels the autoboot, and `run-flashroot.sh` starts with `fastboot reboot` into a fresh loader anyway.
- Polling draws the loader's text over the panel, and `-v` adds the kernel's: screenshots (every 20 polls,
  `logs/<tag>-shot-N.png`) show text over the UI. The `fb_snap`/`fbsnap` route is unaffected.
- `XL_VBAT_MIN` (3700 mV by default) stops a run on a low battery. iOS does not charge: park in pmOS between boots if
  the battery gets low.
- Logs: `research/hackios-xnu-kernel/logs/gpu-<step>-<HHMM>.log` (and `-at-N.log` every 20 polls, since the ring wraps).

### Boot 1: steps 1 + 2 + 3a together (`gpu-run.sh run probe`)

```sh
cd ~/Dev/mobile
GEOM="…" darwin-lisa/kernel/drivers/gpu/gpu-run.sh print probe     # check the line
GEOM="…" darwin-lisa/kernel/drivers/gpu/gpu-run.sh run probe
L=$(ls -t research/hackios-xnu-kernel/logs/gpu-probe-*.log | grep -v -- -at- | head -1)
```

Adds `gpu=1 metal=1 metal_plugin=HakiosMetalProbe init_run=…;/usr/local/bin/gputest init_run_secs=150`. The window
server is not shown the device, so the boot continues to the home screen in software: if steps 1-3a fail, this is
still a normal boot.

**Step 1, the GPU up with the built-in firmware**

```sh
grep -a 'adreno:' $L | grep -E 'built into the kernel|step [0-9]+: (done|FAILED)|IS ALIVE|FIRMWARE IS LOADED|ZAP SHADER|TAKES COMMANDS|0x600dc0de|STAYS ON|NOT STARTED'
```

Pass: the four `fw/a660_*: N bytes built into the kernel`, steps up to 21 `done`, `THE GPU BLOCK IS ALIVE`,
`THE GMU'S FIRMWARE IS LOADED`, `TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER`, `THE 3D CORE TAKES COMMANDS`,
`the page reads 0x600dc0de`, `THE GPU STAYS ON for the system to draw with: the service HakiosGPU`.
Fail lines: `THE GPU IS NOT STARTED: step N (…) failed`; `gpu=1 is for the phone, and this is not it` (the SoC base;
it read 0x400000 on the 26 kernel, so not expected). If the zap is refused (step 19), boot 1 again with the pmOS
set: `XL_FW_RAMDISK=userland-26/out/fw-gpu-pmos-pad32.apfs` (a 32 MiB upload; the RAM disk is asked before the
built-in set, and the log then says `in the image of /dev/md0`).

**Step 2, the probe's triangles** (needs step 1)

```sh
grep -a -E 'init26: RUN .*gputest|gputest:|HakiosGPU: submission|probe: (ok|FAIL)|probe: [0-9]+ failure' $L | head -80
```

Pass: `HakiosGPU: submission 1: …`, `probe: ok` lines for the 1 MB copy, the fills, the command-buffer chain, the
precompiled kernels, the textures and stage 9's triangles, then `probe: 0 failures` and
`gputest: /usr/local/bin/hakios-metal-probe: exit status 0`. Stages that compile on the phone print
`no compiler in this build` (the probe has no Mesa): not failures. A hang: `gputest: … still running after 90 s,
killed`; the kernel's `HakiosGPU: … no answer` says the GPU stopped.

**Step 3a, Metal's device** (needs step 1)

```sh
grep -a -E 'MiniIOMobileFramebuffer: metal=1|metal_test:|HakiosMetal\[' $L | head -60
```

Pass: `the accelerator names HakiosMetalProbe as its Metal plug-in`; in metal_test:
`IOAcceleratorES "sgx": MetalPluginName HakiosMetalProbe, MetalPluginClassName HakiosMetalDevice`,
`MTLCreateSystemDefaultDevice() = "…", class HakiosMetalDevice`, a buffer `… 0 words wrong`, `blits WORK`;
then `gputest: done: … metal_test 0`. Every `not implemented` line of the plug-in names a selector iOS 26 sends:
write them down, they are the next fixes. metal_test's CoreImage part may end killed (no XPC before launchd): not
part of the pass.

Also check the boot went on: the home screen in the last screenshots, `MiniIOMobileFramebuffer: N frames …` lines
as usual, and no `HakiosMetal[backboardd …]` line (the window server was not given the device).

### Boot 2: step 3b, CoreAnimation on Metal (`gpu-run.sh run ca`)

Only after boot 1 passed steps 1 and 2.

```sh
GEOM="…" darwin-lisa/kernel/drivers/gpu/gpu-run.sh run ca
L=$(ls -t research/hackios-xnu-kernel/logs/gpu-ca-*.log | grep -v -- -at- | head -1)
grep -a -E 'HakiosMetal\[backboardd|function constants:|HakiosGPU: [0-9]+ submissions|fps:|MiniIOMobileFramebuffer: [0-9]+ frames' $L | head -60
grep -a -E 'backboardd.*(crash|exited|abort)|DLISA abort.*backboardd|metallib_url' $L | head
```

Adds `gpu=1 metal=1 dlisa_async=1 metal_fps=1` (and `metal_defer=1` when it fits). backboardd loads HakiosMetal.bundle.

Pass, in order:
1. `HakiosMetal[backboardd …]: …` lines: the plug-in loaded in the window server, the device made, a queue.
2. `function constants: <name> and the next ones keep their descriptor's values beside them (iOS 26)`: CoreAnimation's
   uber shaders got their constants.
3. `HakiosGPU: N submissions in … ms` every 5 s, and `MiniIOMobileFramebuffer: … frames …, after N us waiting for the GPU`
   with N above 0: frames come from the GPU.
4. `HakiosMetal[backboardd …]: fps: X frames/s …` every 5 s (`metal_fps=1`).
5. No backboardd restarts (launchd's `crashed N times`, `DLISA abort` lines), and no `metallib_url != nil`.
6. Screenshots: the lock or home screen drawn correctly, not black, not garbled.

If it fails: the first refusal of the plug-in (`HakiosMetal[backboardd …]: … refused` / `not implemented`) and the
last `DLISA abort` say where. The boot-args alone return to software: the next boot without `metal=1` is the
known-good one.

### Contingency boots (only if boot 2 shows the matching symptom)

| Symptom in boot 2 | Boot | Changes |
|---|---|---|
| drawn, but slow or the blur passes refuse | `gpu-run.sh run ca-lite` | `ui_lite=1`: Reduce Transparency and Motion |
| shapes drawn wrong (uber shaders with zero constants) | `gpu-run.sh run ca-native` | `metal_fc_native=1`: Metal's own specialisation tried first |
| zap refused in boot 1 | boot 1 again with `XL_FW_RAMDISK=…/fw-gpu-pmos-pad32.apfs` | the pmOS firmware set |

## Rollback

No GPU boot-arg means no GPU: the vendor_boot's autoboot line has none, so a cold boot is today's boot. The root
rollback is `system-26.apfs.full-199a4181` (the same flash command). The kernel without HakiosGPU is
`kernel-16k-present2.bak` (c54833dc).

## Where the pieces are

`darwin-lisa/kernel/drivers/gpu/`: `README.md` (the iOS 26 analysis), `gpu-wire.py` (kernel), `gpu-run.sh` (these
boots), `userland/` (the build scripts, `gputest.c`, the plug-in and compiler patches, `ca-metallib-stage.sh`),
`mesa/` (the Darwin Mesa build), `fw-gpu-stage.py`, `tools/metal-initlimits.py`.
