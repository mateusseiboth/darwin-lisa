# GPU (Adreno 642L) for iOS 26

Offline preparation, 2026-11-10. Nothing here is wired into the kernel tree or the image yet, and none of it has run on
the phone. The kernel half of the Adreno bring-up (`../hakios.adreno.c`, tag `hakios_adreno`) is already in today's
iOS 26 kernel; this directory holds the rest.

## Files

| File | What |
| --- | --- |
| `hakios.gpu.cpp` | HakiOS's HakiosGPU service and user client, ported to xnu-12377. Two changes from HakiOS (see its header). Compiled with the exact flags of `MiniIOSurface.cpp` from the 26 build record (`-fsyntax-only`, clang-24 in the toolchain image): 0 errors, only the `-Weverything` warnings every IOKit file here has. |
| `gpu-wire.py` | Wires it into xnu-work as tag `hakios_gpu`. Puts the no-op `HakiosGPUStart/Stop` (adreno glue) and `MiniIOSurfaceDrawn` (framebuffer) under `#ifndef HAKIOS_GPU`. Idempotent, `XNU_WORK=` for a dry run. Dry-run twice on a copy of the tree: clean. Not applied. |
| `fw-gpu-stage.py` | Firmware: `stage` (pmOS set, split), `check DIR` (the driver's own checks), `install DIR DEST` (into the image's `fw/`). |
| `fb-wcomb.patch` | Not GPU, not applied: maps the panel write-combined, as HakiOS's tree does. See "CPU load". |
| `userland/build-gpu-probe.sh` | `hakios-metal-probe` for iOS 26 (iPhoneOS 26.5 SDK). Built. |
| `userland/build-hakios-metal.sh` | `HakiosMetal.bundle` (arm64e label) + `metal_test`, `PLUGIN=probe` or `PLUGIN=ca`. Both built, without Mesa. |

Wire order on a fresh tree: `../adreno-wire.py`, `../display/display-wire.py`, then `gpu-wire.py` last. Both earlier
scripts rewrite a file `gpu-wire.py` guards, so after rerunning either one, rerun `gpu-wire.py` too.

The iOS 26.5 SDK is a sparse checkout of github.com/xybp888/iOS-SDKs at `~/Dev/mobile/research/iOS-SDKs/iPhoneOS26.5.sdk`
(328 MB; `git sparse-checkout set iPhoneOS26.X.sdk` there for another one). Its libc++ headers need a newer clang than
the toolchain image's 18, so the scripts take the C++ headers from the 16.5 SDK and everything else from 26.5.

## What iOS 26 changed (read from the 23F84 dyld cache)

Addresses are in the 26.5.2 iPad17,1 cache. `ipsw` needs a `.symbols` file this extraction lacks; a copy of the main
header with the expected UUID written at 0x58 is enough for `dyld extract`, `disass` and `class-dump`.

**The OpenGL ES path is gone from the window server.** On 16.5, `CA::WindowServer::EAGLServer` tried Metal, then
`EAGLContext initWithAPI:3/2 properties:` (0x1882e26c0 in the iPhone10,3 cache), then software. HakiOS's default GPU
path lived in that middle step (`libhakios.gl.dylib` replacing EAGL with Mesa/OSMesa). On 26 the server is
`AccelServer`: `sub_183f35d3c` asks `MTLCreateSystemDefaultDevice()` once (`sub_1842137a0`) and builds a MetalContext
labelled `com.apple.coreanimation.AccelServer`. With no device it returns nothing and the render path tail-calls the
software renderer (`sub_184069190` -> `sub_1841b98ac` -> `sub_1841b994c`, which allocates the 0x5220-byte software
context and logs the old fault "Failed to initialize Metal or OpenGLES contexts!"). `initWithAPI:properties:` and
`_CARenderOGL*` are absent from 26's QuartzCore; `CA_ENABLE_OGL` is read and its value discarded. So on iOS 26 the
only GPU route for backboardd is Metal. That is what draws the lock screen in software today.

**Metal device discovery is the same protocol.** `-[MTLIOAccelServiceGlobalContext init]` iterates `IOAcceleratorES`,
reads `MetalPluginName`, loads `/System/Library/Extensions/<name>.bundle`, takes `MetalPluginClassName` (or the
principal class), requires a subclass of `_MTLDevice`, then `[[cls alloc] initWithAcceleratorPort:]` and
`MTLAddDevice`. New: creation goes through an `MTLIOAccelService` record (`initWithAcceleratorPort:deviceClass:`) and
`MTLAddDevice` also sends `initWorkarounds` (implemented by `_MTLDevice`). The two protocol asserts (`MTLDevice`,
`MTLDeviceSPI`) are unchanged. The kernel's `IOAcceleratorES` stand-in with `metal=1` (display driver) is already right.

**Private Metal ABI the HakiOS plug-in uses.** `class-dump --diff` of Metal 306.6.4 (16.5) against 373.2 (26): every
private selector, class and ivar `HakiosMetalCA.m` relies on is still there (`initWithQueue:retainedReferences:
synchronousDebugMode:`, `initWithDevice:pipelineStateDescriptor:`, `bitcodeData`, `targetDeviceInfo`,
`commitWithDeadline:`, `_commandBufferSemaphore`, `_needsCommandBufferSemaphoreSignal`, `_MTLPipelineCache`, ...).
`_MTLDevice` gained about 150 methods (MTL4, residency sets, tensors); the plug-in's forwarding logs each one it is
asked. One real break: `-[_MTLFunctionInternal baseFunctionConstantValues]` is removed, and `hm_function_constants_declared`
returns 0 constants without a word when it is missing. CoreAnimation 26 also makes functions with
`newFunctionWithDescriptor:error:` (0x183fef134), which the plug-in does not hook (it hooks
`newFunctionWithName:constantValues:error:`). Both must change before CA's uber shaders compile right.

**QuartzCore now ships its shaders.** 16.5 had no `default.metallib` (HakiOS needed `tools/ca-metallib.py` and a Mac's
library). 26 has `/System/Cryptexes/OS/.../QuartzCore.framework/default.metallib`, a fat file with an AIR64 slice
(1.5 MB, 189 functions: 37 vertex, 128 fragment, 24 kernel, AIR 2.8.4, opaque pointers) and an AGX G17 binary slice.
Precompiled pipelines (`newBinaryArchiveWithDescriptor:`) and tile pipelines sit behind `supportsFamily:` checks, so a
device that answers NO for the Apple families gets the AIR path.

**HakiOS's AIR front end on that library** (`tools/air/corpus.py`, host build in the scratchpad): the reader takes all
189 modules, translation to NIR reaches 157 (all 37 vertex, 116 of 128 fragment, 4 of 24 kernels). The 32 that stop:
16 tile/imageblock shaders CoreAnimation only builds for Apple4+ GPUs, and 16 on the `air.get_read_sampler` intrinsic
(SDF generation, rim/brim outlines, variable blur, APL/luma). All 12 Liquid Glass shaders (`glass_*`) translate. ir3
was not measured here (the host has no libdrm headers); on the 16.5 corpus every module that translated also compiled.
The `fixed_vert/fixed_frag` uber shaders read function constants, which is why the break above matters.

**Kernel interfaces.** The IOSurface and IOMobileFramebuffer layouts were already ported (`../display/README.md`).
HakiosGPU's interface is ours on both ends, so it needs no layout change. The 26 framebuffer driver already has every
GPU hook the 16.5 one had (`MiniIOSurfaceDrawn` waits, `IOAcceleratorES`); only the stub had to go.

**Can HakiOS's approach carry over?** Yes, through Metal only. Kernel: HakiosGPU as ported here. Userland: HakiosMetal
over hakios-metal, compiled arm64 against the 26.5 SDK and labelled arm64e, as `libdlisa.dylib` already is. The OpenGL
route (`libhakios.gl`, `eagl.c`, OSMesa) has nothing left to hook in backboardd. The second route HakiOS ran on 16.5,
UniXNU's AdrenoMetal (metal2vulkan + Turnip through `tools/mesa/hakios-drm`), stays a fallback: UniXNU builds it arm64e
for iPadOS 27 (`scripts/build-adreno-metal-plugin.py --target ios27`), the closest existing build to 26's QuartzCore,
but on 16.5 it ran at about half hakios-metal's rate (50-57 against 100-104 fps in swipes).

## CPU load today

`cpu-b3-smp7-vnodes.log` (`cpuload=5000`, 7 cores, lock screen): cores 63-72% busy, and every 5 s
`MiniIOMobileFramebuffer: 56 frames in 5015 ms (11 a second), 77842 us each in the driver, after 0 us waiting for the GPU`,
53 of 56 swaps waiting for the frame before. backboardd is not the bottleneck. The kernel compositor is: 78 ms per frame
against about 9 ms per core on HakiOS 16.5 ("scaling a whole frame takes one processor some 9 ms"). The cause I found:
HakiOS's tree maps the boot framebuffer write-combined (`video_console.c`: `#if HAS_UCNORMAL_MEM || defined(BCM2837)`),
xnu-12377 keeps `HAS_UCNORMAL_MEM || APPLEVIRTUALPLATFORM`, so on this board the panel is Device memory. The same fact
explains the `bzero`/`DC ZVA` alignment panic in `show()` of 2026-11-10. `fb-wcomb.patch` restores HakiOS's line. It is
a one-line kernel change in the display worker's area, so it is left for them. GPU rendering alone would not lift the
11 fps, because every frame still goes through this copy.

The `tl: u` lines give only per-core busy shares, so how much of the rest is backboardd's software rasterizer is not in
the log. `psample=backboardd` on the next run would say.

## Firmware

`fw-gpu/` (the phone's stock blobs, the set that brought the GPU up on 16.5) is unchanged. `fw-gpu-stage.py stage`
put the pmOS set in `research/hackios-xnu-kernel/fw-gpu/pmos/` (git-ignored like the rest of that repository): gmu and
sqe from `lib/firmware/qcom/`, the board zap `qcom/sm7325/xiaomi/lisa/a660_zap.mbn` split into `.mdt` + `.b00..b02`,
with `SHA256SUMS`. No `.zst` was present in the chroot (the script decompresses one if it is). Both sets pass the
driver's checks (mdt 6860 = 52 + 32*3 + 6712, b02 1976 <= 4096). The pmOS `a660_zap.b02` is byte-identical to the
stock one; the `.mdt` hash segment, the gmu (55252 vs 55256) and the sqe (43292 vs 40496) differ. Use the stock set
first; the pmOS one is the fallback.

The driver reads `fw/` of md1, else md0. On the 26 boot md0 is the system image, so the files go in
`userland-26/out/sysroot-26/fw/` next to the Wi-Fi firmware. `pe_arm_get_soc_base_phys()` is 0x400000 on the 26 kernel
(`soc_phys: 0x400000` in the log), so the `gpu=1` gate passes.

## Device test plan

Base arguments are those of the last good run (`cpu-b3`). Every run starts from `research/hackios-xnu-kernel`:

```sh
BASE="display=1 console_open=1 keystore=1 wlan_stub=1 cpufreq=1 cpufreq_max=2400 cpus=7 cpuload=5000"
UM="/usr/libexec/usermanagerd:--init"
IMG=/home/mateusseiboth/Dev/mobile/userland-26/out/system-26.apfs.trim
```

### 0. Image (once, after the phone and the image builder are free)

```sh
python3 ~/Dev/mobile/darwin-lisa/kernel/drivers/gpu/fw-gpu-stage.py install \
    ~/Dev/mobile/research/hackios-xnu-kernel/fw-gpu ~/Dev/mobile/userland-26/out/sysroot-26/fw
~/Dev/mobile/darwin-lisa/kernel/drivers/gpu/userland/build-gpu-probe.sh
PLUGIN=probe ~/Dev/mobile/darwin-lisa/kernel/drivers/gpu/userland/build-hakios-metal.sh
(cd ~/Dev/mobile/userland-26 && LEAN=1 bash scripts/mkimage-trim.sh)
```

### 1. GPU probe and firmware load. Today's kernel, no rebuild

```sh
XL_SECS=600 XL_POLLS=40 XL_EXTRA_ARGS="$BASE gpu=1 init_run=$UM init_run_secs=60" ./run-fullroot.sh $IMG
grep -a "adreno:" logs/fullroot.log | tail -40
```

Pass: steps 1-21 `done`, `TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER` (19), `the page reads 0x600dc0de`
(21), `THE GPU STAYS ON for the system to draw with`. With today's glue no HakiosGPU service appears yet; that is
expected. Fail lines: `THE GPU IS NOT STARTED: step N (...) failed` or `the small image has no fw/...`. If the log ring
loses the lines (it did for touch), add `adreno_pause=0 adreno_watch=0` and read earlier polls; for a step-by-step
look, `adreno=21 adreno_skip=0x1c00 adreno_keep=1` instead of `gpu=1`. If the zap is refused, swap in the pmOS set
(`fw-gpu-stage.py install .../fw-gpu/pmos ...`). Then confirm the boot still reaches the lock screen
(`XL_SHOT_EVERY=10`).

### 2. Triangle and clear on the GPU. Needs the hakios_gpu kernel

```sh
# in docker as root, like the other wire scripts; then the kernel rebuild (the other worker's .work/run-rebuild.sh)
python3 ~/Dev/mobile/darwin-lisa/kernel/drivers/gpu/gpu-wire.py
XL_SECS=600 XL_EXTRA_ARGS="$BASE gpu=1 init_run=$UM;/usr/local/bin/hakios-metal-probe init_run_secs=120" \
    ./run-fullroot.sh $IMG
grep -a "probe:\|HakiosGPU" logs/fullroot.log
```

Pass: `HakiosGPU: submission 1: ...`, then `probe: ok` lines for the 1 MB copy, the fills, the chained command
buffers, the precompiled compute kernels, the textures, and stage 9's 64x64 triangles checked pixel by pixel; the last
line counts failures (the exit status too). The stages that compile on the phone print "no compiler in this build".
`--bisect` and `--recover` are the probe's own switches for a hang (`gpu_recover=1` on the kernel side).

### 3. Metal, then CoreAnimation on the GPU

3a. Discovery, safe for the window server (PLUGIN=probe image): add `metal=1` and run `metal_test`:

```sh
XL_SECS=600 XL_EXTRA_ARGS="$BASE gpu=1 metal=1 init_run=$UM;/usr/local/bin/metal_test init_run_secs=120" ./run-fullroot.sh $IMG
grep -a "HakiosMetal\|metal_test\|MiniIOMobileFramebuffer: metal" logs/fullroot.log
```

Pass: `the accelerator names HakiosMetal`, `HakiosMetal: the class HakiosMetalDevice is registered`, metal_test prints
the device, its queue, a buffer round trip and a blit/fill on the GPU. Every `not implemented` line names a 26-only
selector to answer. backboardd is not shown the device at `metal=1` and keeps drawing in software.

3b. CoreAnimation. Not buildable yet; it needs, in order: `tools/mk-mesa.sh` (Darwin Mesa 25.0.7 with the AIR front
end), `make ios MESA=1` in hakios-metal, the plug-in changes listed under "Work left", then
`PLUGIN=ca build-hakios-metal.sh` and the image. Boot with
`$BASE gpu=1 metal=1 metal_fps=1 metal_defer=1 touch_lat=1 dlisa_oslog=backboardd`. Look for: backboardd loads the
bundle and does not restart, no `metallib_url != nil` assertion (if it appears, copy QuartzCore's `default.metallib`
from the OS cryptex into `/System/Library/Frameworks/QuartzCore.framework/` of the sysroot), the `HakiosGPU: N
submissions` stats every 5 s, framebuffer stats with non-zero `waiting for the GPU`, the plug-in's `fps:` lines, and a
correct lock screen in `XL_SHOT_EVERY` shots. `ui_lite=1` (Reduce Transparency/Motion) takes Liquid Glass's blur out
of the first runs.

## Work left, and how big it is

| Piece | Where | Size |
| --- | --- | --- |
| Step 1 on the phone | device | one session; the driver and firmware are the ones that passed 21/21 on 16.5 |
| Wire + rebuild + step 2 | kernel build, device | one session |
| Step 3a | device | one or two sessions, mostly reading which 26 selectors Metal sends |
| Darwin Mesa build with the AIR front end | offline (docker) | about a day. mk-mesa.sh wants `xnu-deps/host/bin/ld64 -> ld64-lld`, the same symlink the 16.5 kernel build needs pointing at cctools; use a private copy of `host/bin` instead of flipping it under the other worker |
| HakiosMetalCA.m for 26 | offline | 2-4 days: function constants from `MTLFunctionDescriptor` / `newFunctionWithDescriptor:error:` (the removed `baseFunctionConstantValues`), drop the 16.5-only pipelinelib, label-variant and `ca_metallib` code, answer the new `_MTLDevice`/`MTLDeviceSPI` queries CA makes |
| AIR front end | offline (host build) | `air.get_read_sampler` (16 modules); then ir3 and CPU-reference checks over the 157 |
| CoreAnimation on Metal on the phone | device | the open-ended part: HakiOS needed about a week of device iterations on 16.5 (27 Oct to 3-4 Nov) from "device exists" to the window server drawing. Expect 1-3 weeks on 26 |

Total: a few days to step 3a, then two to four weeks to a GPU-composited home screen, if nothing below bites hard.

## Risks

- `featureProfile` and `initLimits`. On 16.5 only profile 0 filled the limits table and 2, 3, 10-12 aborted. 26's
  `initLimits` reads the same selector, but its table was not decoded here. The first sign is an abort in `MTLAddDevice`.
- Liquid Glass. Many full-screen blur and refraction passes at 1125x2436 on a 550 MHz Adreno 642L. The SDF and
  variable-blur compute shaders do not translate yet. `ui_lite=1` and `fbscale=2` are the fallbacks.
- The present copy. Until `fb-wcomb.patch` (or a GPU blit to the panel) lands, the frame rate stays near 11 fps
  whatever renders.
- Memory. User space already takes about 1.6 GB with the root in RAM. Mesa adds 14 MB per client and HakiosGPU wires
  buffers (up to 160 MB kept per client).
- Function constants silently zero. Shaders then compile and draw wrong instead of failing, so screenshots, not logs,
  catch it.
- Process restarts. A Metal device that exists but fails makes launchd restart backboardd over and over (88 crashes in
  100 s on 16.5). Keep `metal=1` with the probe plug-in until 3a is clean.
- Log capture. On 16.5 the adreno output pushed touch lines out of the loader's ring buffer. Run steps 1-2 without the
  touch arguments.
- The step-1 assumption that today's 26 kernel behaves like HakiOS's under the hypervisor with 7 cores and the 16 KB
  page. adreno.c already sizes its SMMU tables for 16 KB pages, and HakiOS ran the GPU with `cpus=8`. It is still the
  first thing the phone has to confirm.
