# GPU prior art: Metal on Adreno under XNU

Research of 2026-11-16, read only. Question: what already exists in public that can serve as the base for the
next GPU core of darwin-lisa (iOS 26.5.2, xnu-12377, lisa / SM7325 / Adreno 642L, chip id `0x06030500`), in
particular for the idea of replacing our own core (AIR to NIR to ir3 plus a hand-written C++ command layer) with
Turnip.

Every repository below was opened (README at least; the ones marked "cloned" were read in source). Commits are the
ones I read. Nothing was built or run. Stars and dates come from the GitHub API on 2026-11-16. Clones for reading are in
the session scratchpad, not in this repo.

## Short answer

Turnip as the core is the road almost every public port took in the last month, and it already runs on our exact
GPU. The stack is:

```
Metal.framework -> plug-in (MTLAddDevice) -> metal2vulkan runtime + AIR->SPIR-V translator
                -> Turnip (Mesa freedreno Vulkan) with a Darwin kernel backend
                -> IOKit user client (NativeAdreno ABI v1) -> XNU Adreno driver
```

- UniXNU publishes all of it under permissive licences except metal2vulkan (GPL-2.0-or-later OR LGPL-3.0-or-later).
- **RigoLigoRLC/unixnu-adreno6xx** is the UniXNU kernel driver ported to the **Adreno 642L on SM7325** (Galaxy Tab
  S7 FE). On 2026-11-04 it composited the iPadOS 17.7.11 desktop through Metal with that stack.
- **IOS-26-Hot** runs the same stack on **iOS 26.5.2 arm64e** on a Snapdragon 855 (Adreno 640, a6xx, Cortex-A76 with
  no PAC). It is the closest public match to our combination of OS version, GPU generation and CPU.
- One measurement goes against it. HakiOS ran metal2vulkan + Turnip on the same A642L (redwood, iOS 16.5) and got
  50 to 57 fps on page swipes, against 101 for hakios-metal (the lineage of our current driver). The conditions were
  not the same and that run used HakiOS's msm-DRM emulation, not UniXNU's kernel side. Other ports report 95 to 110
  fps with the UniXNU stack on 7xx/8xx GPUs.

Recommendation at the end.

## 1. iOS / iPadOS ports on Snapdragon, and how they do the GPU

### The UniXNU stack (the reference everyone forks)

| Repo | What | Licence | State |
| --- | --- | --- | --- |
| [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) (cloned, `81e6460`, 2026-11-17) | Build system, OS profiles (`ipados-17.7.11-21H461`, `ipados-26.5.2-23F84`, `ipados-27.0.1-24A446`, `macos-27.3`), Mesa patches, build scripts | BSD-2-Clause-Patent | 2943 stars, very active |
| [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) (cloned, `9820d00`) | The Metal plug-in: `NativeAdrenoMetalPlugin.mm`, `NativeBridgeMetalFrontend.mm` (5.8k lines of Objective-C++ front end), `bridge/NativeMetalBridge.rs` (metal2vulkan runtime behind a C ABI), 17 metal2vulkan patches | MIT | "GPU composition verified on sheng with A740" |
| [UniXNU/Adreno740](https://github.com/UniXNU/Adreno740) (cloned) | XNU IOKit driver: GMU/HFI, CP rings, SMMU 4K tables, BOs, fences, two-ring preemption | MIT | Runs the sheng (SM8550) desktop |
| [UniXNU/AdrenoPM4](https://github.com/UniXNU/AdrenoPM4), [AdrenoFirmware](https://github.com/UniXNU/AdrenoFirmware), [ArmSmmu4K](https://github.com/UniXNU/ArmSmmu4K) | Kits linked into the kernel driver | MIT | Used by every UniXNU GPU port |
| [UniXNU/AirNir](https://github.com/UniXNU/AirNir) (cloned) | Offline AIR to NIR to ir3 compiler kit | MIT | Offline only; the docs say the runtime uses metal2vulkan |

How the pieces connect:

- `patches/mesa/turnip-darwin-kmd.patch` (78 KB) adds `src/freedreno/vulkan/tu_knl_darwin.cc` and the meson option
  `-Dfreedreno-kmds=darwin`, `-Dtu-darwin-abi-dir=<kernel driver dir>`. Turnip talks to the kernel driver's IOKit
  user client with the fixed records of `NativeAdrenoABI.h` (BO alloc/import/map, submit, wait fence, timestamps). It
  fills Turnip's UBWC config (`highest_bank_bit`, swizzle, macrotile) from the driver's DeviceInfo, so Turnip's own
  images can be UBWC.
- Mesa pin: `c823a83e` (main). metal2vulkan pin: `963f0e64` on `development`. Recorded in
  `scripts/data/gpu-metal-vulkan-inputs.json`.
- GMEM and frame-cost work, as Turnip patches: `turnip-transient-attachments-in-gmem.patch` (memoryless attachments
  stay in tile memory under `TU_DEBUG=sysmem`), `turnip-gmem-mutable-swap.patch`, `turnip-render-mode-for-attachment-memory.patch`,
  `turnip-builtin-shader-pack.patch`, `turnip-current-queue-priorities.patch`, frame diagnostics.
- Shader warm-up: a priming list of every `.metallib` plus exported pipeline packs (`make-m2vpack.py`), keyed by the
  translator build and Turnip's `LC_UUID`. `docs/porting/gpu.md` describes it.
- `docs/porting/gpu.md` has the bring-up order and the per-call cost rules for map/unmap/submit, which match the
  problems we hit.
- Build targets are `ios` (iPadOS 17), `ios27` and `macos27`. No `ios26` target upstream. The iOS 26 ports below
  built it for 26 themselves.

### Ports on our SoC (SM7325 / Adreno 642L)

| Repo | What | GPU path | State | Licence |
| --- | --- | --- | --- | --- |
| [RigoLigoRLC/unixnu-adreno6xx](https://github.com/RigoLigoRLC/unixnu-adreno6xx) (cloned, `86e0f1b`, 2026-10-31) | Adreno740 rewritten for the **A642L**: chip id `0x06030500`, 512 KiB GMEM, A660 firmware (`a660_gmu.bin`, `a660_sqe.fw`, A660 ZAP), Yupik GPUCC/GCC/RSCC/PDC power, HFI A6xx queues, LPAC context, DVFS 315/450/550 MHz with paired DDR votes, async submit (64 jobs, 8 in the ring), perf counters | Keeps **NativeAdreno ABI v1**, so the UniXNU Turnip and AdrenoMetal run unchanged | 2026-10-25: Turnip fill, compute and triangle exact on hardware, DeviceInfo UBWC=3, swizzle=6 | MIT for the inherited code; files with Qualcomm A6xx/Yupik tables are GPL-2.0-only (from Samsung's SM7325 kernel) |
| [RigoLigoRLC/unixnu-sm-t733](https://github.com/RigoLigoRLC/unixnu-sm-t733) (cloned, `e8d584c`, 2026-11-08) | Galaxy Tab S7 FE device repo | Above + [unixnu-adrenometal](https://github.com/RigoLigoRLC/unixnu-adrenometal) (MIT) + metal2vulkan + Turnip; DPU scans the IOSurface directly | iPadOS **17.7.11**. Build 76b (2026-11-04): Metal accelerator published, SpringBoard/backboardd running, 1247 display completions, 0 errors. No fps figure published | BSD (device), modules keep theirs |
| [RigoLigoRLC/unixnu-sm7325-common](https://github.com/RigoLigoRLC/unixnu-sm7325-common) | SM7325 SoC layer for UniXNU | | Used by the T733 build | UniXNU's |
| [ETCHDEV/IOS-LISA](https://github.com/ETCHDEV/IOS-LISA) (cloned, `d5a7c0e`, 2026-11-05) | **iOS 26.5.2 on lisa itself**: xnu-12377.121.6, userland rewritten for no PAC, RAM boot | Plan (`IOS26-PORT-PLAN.md`): drop their old AIR to Gallium path, use NativeMetal + metal2vulkan + Turnip over their own LisaGPU | Display by CPU copy (~105 ms/frame). GPU "not confirmed driving the UI". On iOS 16 they proved Turnip, metal2vulkan compute, render and no-copy import | None chosen |
| [Abhi-Flex1/ios-lisa](https://github.com/Abhi-Flex1/ios-lisa) | iOS on lisa | Listed only, not read | | |

The T733 repo has two documents worth reading before any design: `docs/A642L_METAL_FAMILY_RESEARCH_2026-10-28.md`
(which Apple GPU family the A642L can honestly claim with Turnip: Apple3 is the first complete target, Apple4 needs
tile/imageblock/raster-order work) and `docs/GPU_PIPELINE_CACHE_DESIGN_2026-10-27.md`.

### iOS 26 ports with a GPU

| Repo | SoC / GPU | CPU PAC | GPU path | Reported state | Licence |
| --- | --- | --- | --- | --- | --- |
| [nithinyadavamaraboina-online/IOS-26-Hot](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot) (cloned, `d249939`, 2026-11-01) | SM8150 / **Adreno 640** | **No** (A76; PAC trap fast path in `locore.s` + offline de-PAC) | `HotMetal2.bundle` = UniXNU `NativeBridgeMetalFrontend.mm` + metal2vulkan + Turnip; kernel exposes NativeAdreno ABI v1 (patch 0115) | iOS 26.5.2 Liquid Glass composited; 52.9 fps home swipe on a 60 Hz panel; one Turnip patch (`0100-sweet-fetch-gmem-min-draws.patch`) | MIT |
| [superboyyy/hakios-arm64e-dada](https://github.com/superboyyy/hakios-arm64e-dada) (cloned) | SM8750 / Adreno 830 | Yes | `gpu2/`: patches to metal2vulkan (`720ef1c`, also tested at upstream `0dfdbcd`), UniXNU NativeMetal (`c6d29ca`) and Mesa (`85fda3b`); Turnip built with `-Dfreedreno-kmds=darwin` for iOS 26 arm64e | Home screen, GPU on; Liquid Glass core shaders pixel-exact against an Apple GPU, some issues left | APSL / MIT / patch licences per project |
| [hchyhchyxh/hakios-arm64e-xuanwu](https://github.com/hchyhchyxh/hakios-arm64e-xuanwu) | SM8750 / A830 | Yes | dada lineage | Metal device discovered, kernel side missing; lock screen clock faked with a label | |
| [marnov999/ios26-sweet](https://github.com/marnov999/ios26-sweet) | SM7150, SM8150 | No (whole shared cache lowered to arm64 offline) | Planned: Metal on Adreno (states CoreAnimation 26 has no GL ES fallback) | Not there yet | |
| [HouseOfBalance/ios26-sapphire](https://github.com/HouseOfBalance/ios26-sapphire) | SM6225 / A610 | No (trap-and-self-patch PAC) | Metal, metal2vulkan, Turnip, NativeAdreno KMD, plus HakiOS `hakios.adreno.c` | README claims; not checked | |
| [nishant713/ios-pipa](https://github.com/nishant713/ios-pipa) | SM8250 / A650 | No | Mesa freedreno GL ES + hakios-metal + CoreAnimation bridge | Claims iOS 26, "85%" GPU | |
| [TheAndersMadsen/ios-on-pixel-10](https://github.com/TheAndersMadsen/ios-on-pixel-10) | Tensor G5 (PowerVR) | | Own Metal device | Black screen; no GPU frame | No licence |

Note for the project memory: the claim "no public repo runs iOS 26 arm64e on a non-PAC CPU" is out of date.
IOS-26-Hot (A76), ios26-sweet (A76), ios26-sapphire (A73), IOS-LISA (our phone) and
[none24smu/UniXNU-zeta](https://github.com/none24smu/UniXNU-zeta) (SM8350, cited by IOS-LISA as the reference
userland rewrite) all do it, each with its own de-PAC scheme.

### Other UniXNU-lineage GPU ports (7xx/8xx, older iOS)

| Repo | GPU | Result |
| --- | --- | --- |
| [ShueiTsuki/data-a-live](https://github.com/ShueiTsuki/data-a-live) | A840, SM8850, iOS 16 | Own kernel driver + Turnip + NativeMetal + modified metal2vulkan; ~110 fps average swipe on 120 Hz. Its `wsl/pandora_gpu.c` DVFS policy is what adreno6xx copied |
| [qiumochen/UniXNU-zizhan](https://github.com/qiumochen/UniXNU-zizhan) | A730, iPadOS 18 | Turnip + metal2vulkan; 95.3 fps compositing swipe after a fix (baseline 56.7 to 58.4); first frame 75 to 80 ms |
| [Shad0w23333/Adreno830](https://github.com/Shad0w23333/Adreno830) + [UniXNU-OnePlus13T](https://github.com/Shad0w23333/UniXNU-OnePlus13T) | A830, iPadOS 17 | Desktop, apps, widgets, Control Center through Metal (MIT) |
| [xiuyityer/UniXNU-tb371fc](https://github.com/xiuyityer/UniXNU-tb371fc) | **A650** (a6xx), iPadOS 17 | GPU "works", using [xVanTuring/Adreno650](https://github.com/xVanTuring/Adreno650) and AdrenoMetal with 21 translation patches; wallpaper black |
| [xVanTuring/Adreno650](https://github.com/xVanTuring/Adreno650), [xVanTuring/Adreno610](https://github.com/xVanTuring/Adreno610) | a6xx forks of Adreno740 (MIT) | READMEs not updated; source not read |
| [CrackerCat/UniXNU-sweet](https://github.com/CrackerCat/UniXNU-sweet) | A618 | AdrenoFirmware override for A618 legacy HFI; bring-up |
| [QPRS-6/iPadOS18-liuqin](https://github.com/QPRS-6/iPadOS18-liuqin), [Chengdong-Yu/pong-adreno-metal](https://github.com/Chengdong-Yu/pong-adreno-metal), [lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji), [akshaynexus/iOSPlus10](https://github.com/akshaynexus/iOSPlus10) | A730 / A750 | Same stack; houji reports purple shadows, text glitches, dropped frames |
| [iOSNJBrepo/macOS-uke](https://github.com/iOSNJBrepo/macOS-uke) | A732, **macOS 26.5** | Adreno740 driver + AdrenoMetal + Turnip, WindowServer on the GPU; many fence-wait timeouts while the GPU is idle |
| [xAmarnath/iOS-IQOO13](https://github.com/xAmarnath/iOS-IQOO13) | A830 under Gunyah | XNU guest; Metal, metal2vulkan, Turnip, guest virtio-gpu, then the **Android host's stock KGSL** |

### Ports that do the GPU without Vulkan

| Repo | How | Notes |
| --- | --- | --- |
| [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) (sparse clone, `63fc2b4`) `hakios-metal/` ([old repo](https://github.com/oosexgang/hakios-metal), MIT) | Metal emitted as A6xx PM4 directly, shaders AIR to NIR to ir3 in process | Our lineage. Its README still lists GMEM (tiled) rendering, MSAA, stencil and several colour attachments as "not yet" |
| [ry-diffusion/Chlorophyll](https://github.com/ry-diffusion/Chlorophyll) (cloned) | AIR to NIR to ir3, Metal over Mesa's freedreno **Gallium** driver, A630 (Bergamota, iOS 18) | MIT. Same idea as our first driver, over Gallium instead of hand PM4 |
| [ilertnost/Adrenoline](https://github.com/ilertnost/Adrenoline) | metal2nir + a5xx backends + XNU driver, no Vulkan (Turnip is a6xx+) | GPL. Not useful for a6xx |
| [Snapintosh/Snapintosh](https://github.com/Snapintosh/Snapintosh) + [SnapMetal](https://github.com/Snapintosh/SnapMetal) | macOS on Snapdragon X2 Elite: Metal plug-in on hand-written PM4, `air2ir3` compiler, IOGPU kext | MIT plug-in. 1304 stars. Desktop renders correctly; a direct-PM4 design that works at scale |
| [nonrootus/ios-sweet_k6a](https://github.com/nonrootus/ios-sweet_k6a), [limonetexd/ios-sweet-gpu](https://github.com/limonetexd/ios-sweet-gpu), [YiKongk/HackiOS-Ace5](https://github.com/YiKongk/HackiOS-Ace5), [faridbtw/ios-dodge](https://github.com/faridbtw/ios-dodge) | iOS 16 with OpenGL ES (Mesa freedreno/OSMesa behind CoreAnimation's GL renderer), some partial Metal | Not an option on 26: CoreAnimation 26 needs Metal |
| [2myh120405/ios-sweet-mesa](https://github.com/2myh120405/ios-sweet-mesa) | softpipe OSMesa dylib for iOS | Software fallback only |

### HakiOS's own Turnip experiment on the A642L (most relevant measurement)

HakiOS already ran metal2vulkan + Turnip on redwood (same SoC), and the code is in our local checkout
`research/hackios-xnu-kernel`: `tools/mesa/turnip-darwin.patch`, `tools/mesa/hakios-drm/`, `tools/m2v/`,
`tools/mk-turnip.sh`, `tools/mk-m2v.sh`. That route is different from UniXNU's:

- Turnip keeps its **msm** backend. `hakios-drm` stands in for libdrm and turns Linux msm DRM ioctls into calls to
  HakiosGPU's IOKit user client (HakiosGPU answers msm DRM request numbers plus `0x7e`, `0x7d`). So an msm-like
  interface for XNU already exists, as a user-space shim.
- `vk_test` result: "Adreno 7c+ Gen 3", Vulkan 1.3.305, submit + fence wait 0.5 ms best, 2.0 ms average.

From `docs/reference/metal-measurements.md` (iOS 16.5 user space, 1125x2436 scaled to 1080x2400, 550 MHz):

| Motion | hakios-metal | metal2vulkan + Turnip |
| --- | --- | --- |
| Page swipe | 101 (100 to 104) | 50 to 57 |
| Settings scroll | 89 to 91 | 41 |
| Home button | 92 | 41 to 57 |
| GPU time per frame (home) | ~10 ms | 14 to 21 ms |

Their own caveats: the two columns were measured on different days and kernels. On the Vulkan path each frame
rendered to a tiled image and a compute pass merged it into the IOSurface; `TU_DEBUG=gmem` and `sysmem` gave the
same rates; `VK_NV_linear_color_attachment` was unusable because the pass also had a non-linear attachment. That is
the problem UniXNU's later GMEM/transient-attachment patches and in-place IOSurface textures target. The HakiOS run
used AdrenoMetal with 30 patches over metal2vulkan `624bab3`, older than what the UniXNU ports use now.

## 2. Metal on Vulkan (outside the phone ports)

| Project | What | Coverage / maturity | Licence |
| --- | --- | --- | --- |
| [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) (cloned, `d9236a8`, 2026-11-15) | Rust. `metal2vulkan` translator (AIR to SPIR-V + pipeline layout + binding plans), `metal2vulkan-runtime` (Metal API over `ash`), `metal2vulkan-air` (metallib/bitcode reader) | Beta 0.2. `docs/METAL-COVERAGE.md` against the macOS 27.2 SDK: 982 implemented, 804 missing, 349 out of scope. Compute, render, tessellation, tile and mesh pipelines; function constants; argument buffers (tier 2 needs buffer device address; pipeline states/ICBs inside argument buffers refused); framebuffer fetch needs rasterization-order attachment access or fragment shader interlock (`docs/REFUSALS.md`). Typed refusals rather than wrong output. Rust 1.98, Vulkan 1.2 device | **GPL-2.0-or-later OR LGPL-3.0-or-later**. A shipped bundle that links it must offer its source |
| [darlinghq/indium](https://github.com/darlinghq/indium) | Metal-like C++ API on Vulkan 1.3; Iridium translates AIR to SPIR-V at run time; `mtl2spv` CLI | README only; coverage not documented; meant as the base of an ABI-compatible Metal for Darling | 0BSD |
| [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) (cloned) | Metal driver for NVIDIA on macOS 15: own Metal plug-in over Vulkan (`plugin/nvmtl_vk.c`, not the metal2vulkan runtime), AIR to SPIR-V translator based on metal2vulkan, NVK patched to talk to NVIDIA's kernel modules instead of DRM | The same architecture as Turnip under XNU, shipped to users (4432 stars). `docs/HOW-IT-WORKS.md` is a good map | Plug-in and kexts **PolyForm Noncommercial**; translator LGPL-3.0 |
| [watchthelight-labs/WTLIntelGfx](https://github.com/watchthelight-labs/WTLIntelGfx) | Intel Xe on macOS: FreeBSD drm-kmod (i915) built for XNU through LinuxKPI plus a small FreeBSD-API layer; Metal and Vulkan | WindowServer composites on the GPU; hours of uptime | Not checked |
| [ediwiuwj/tahoe-on-linux](https://github.com/ediwiuwj/tahoe-on-linux) | macOS 26 user space on Linux; one AIR kernel through metal2vulkan on Vulkan | Plan only for the window server | MIT |
| [boparaiamrit/macos27-arm64-on-x86](https://github.com/boparaiamrit/macos27-arm64-on-x86) | macOS 27 in QEMU; WindowServer over a metal2vulkan vGPU on lavapipe/Dozen | Research log | MIT/GPL |
| [Fasdeq13/vulkan-cocoa-core](https://github.com/Fasdeq13/vulkan-cocoa-core) | Claims Metal 3 + CoreAnimation to Vulkan 1.3 on XNU | README is marketing; no evidence | Not checked |
| [AhmedZaeem/metal2vulkan](https://github.com/AhmedZaeem/metal2vulkan) | Fork of metal2vulkan, self-described as AI-edited | Skip | |
| [Hi-Jiajun/metal-api-emulator](https://github.com/Hi-Jiajun/metal-api-emulator) | Compute-only prototype, "not a Metal.framework ABI implementation" | Skip | |
| [touchHLE](https://github.com/touchHLE/touchHLE) | HLE emulator for early iOS apps | OpenGL ES only, no Metal | MPL-2.0 |

Not found: an Android iOS-game emulator that translates Metal to Vulkan, a "MetalVK" project, a Wine-like Metal
layer other than Darling's Indium. ipasim and Darling's main tree were not opened.

## 3. AIR / MSL translators

| Direction | Exists | Where |
| --- | --- | --- |
| AIR to SPIR-V | Yes, several | metal2vulkan (most complete, tested against Apple GPUs pixel by pixel by the dada port), Darling Iridium, NullMoth's translator (metal2vulkan-based) |
| AIR to NIR | Yes, out of tree | Our Mesa AIR front end (from HakiOS `tools/mesa/air`), UniXNU AirNir (offline), Chlorophyll `air2nir`, Adrenoline `metal2nir` (a5xx), Snapintosh `air2ir3` |
| AIR to NIR in upstream Mesa | Not found | A web search found no merge request. Mesa has the reverse (KosmicKrisp, Vulkan on Metal, with a NIR to MSL backend) |
| MSL source to SPIR-V | Not found as a tool | SPIRV-Cross only goes SPIR-V to MSL; MoltenVK is Vulkan on Metal. Apple's metal-shaderconverter goes DXIL to metallib. The ports avoid MSL source entirely: iOS ships compiled `.metallib`, and "source compiles" are out of scope in metal2vulkan |

## 4. Adreno kernel interfaces for XNU

| Approach | Example | Comment |
| --- | --- | --- |
| Own IOKit user client with fixed records, Turnip backend `tu_knl_darwin.cc` | UniXNU `NativeAdrenoABI.h` v1: Adreno740, adreno6xx (A642L), Adreno830, IOS-26-Hot, dada | The de facto standard. Turnip's patch is generation-agnostic; adreno6xx proves it on our GPU |
| Emulate msm DRM ioctls in a user-space libdrm shim, Turnip unchanged (msm backend) | HakiOS `hakios-drm` over HakiosGPU | Already in our tree. Fewer Mesa changes, but every DRM concept (syncobj, GEM) has to be faked |
| Use Android's KGSL through a hypervisor | iOS-IQOO13 (Gunyah, virtio-gpu) | Not applicable to a bare-metal XNU |
| Port the Linux driver through a Linux-API layer on XNU | WTLIntelGfx (i915 via LinuxKPI) | Shows it can be done for DRM drivers; nobody has done drm/msm or kgsl this way. Large, and GPL |

No port of kgsl proper (`/dev/kgsl-3d0` ioctls) to XNU was found. IOS-26-Hot has a `/dev/sweet_gpu` + `/dev/pmem`
pair for its older paths, but its Turnip path uses NativeAdreno ABI.

## 5. HakiOS by third parties

- Press in October 2026 (Decrypt, Android Authority, smhn.info, eTeknix and others) about HakiOS running iOS 16.5 on
  the POCO X5 Pro, mostly restating the README.
- The [project site](https://hackios-web.vercel.app/en) describes hakios-metal as "Metal into the Adreno's own command
  packets, no Vulkan in between".
- Dozens of device forks on GitHub (Mi 10, Mi 9, OnePlus 6T, Pixel 7, MediaTek and Exynos phones), almost all on iOS
  16.5 with the GL ES or hakios-metal path. The iOS 26 descendants are dada, xuanwu and sapphire (above).
- Nothing on Reddit, XDA, 4pda or Telegram turned up in the searches I ran. Not confirmed either way.

## What we can reuse

1. **Kernel side.** `RigoLigoRLC/unixnu-adreno6xx` is a complete A642L driver on the exact firmware set we load
   (A660 GMU/SQE/ZAP). Even if we keep `hakios.adreno.c`, its power sequence, DVFS table (speed bin 117: 315/450/550
   MHz with DDR indexes 3/6/9), GMU OOB perf-counter handshake and async submit design are worth diffing against
   ours. Mind the GPL-2.0-only files.
2. **Turnip backend.** UniXNU `turnip-darwin-kmd.patch` plus the GMEM/transient-attachment patches. If our kernel
   implements NativeAdreno ABI v1, Turnip, AdrenoMetal and every fix the community makes apply to us unchanged.
3. **Front end.** AdrenoMetal (MIT) as the plug-in, with dada's and IOS-26-Hot's iOS 26 changes. dada's `gpu2/`
   build scripts already cross-build Turnip and the bridge for iOS 26 arm64e (`build_turnip_ios.sh`, Rust target
   `arm64e-apple-ios-pauth.json`). We would build it as plain arm64 or keep the arm64e label, as with our current
   bundle.
4. **Shader cache and priming.** UniXNU's priming list and pipeline packs cover the cold-compile stalls we fight with
   `hakios-metal-cache`.
5. **Measurement.** adreno6xx's capture (counters, per-job CP timestamps, frame-to-writer matching) is a ready
   profiler for the A642L.

## Recommendation

Take the colleague's direction, but in UniXNU's form, not HakiOS's msm shim:

- Expose NativeAdreno ABI v1 from our kernel, either by porting adreno6xx's user client onto our driver or by
  adopting adreno6xx with our board inputs. Then build UniXNU's Turnip (darwin KMD) and AdrenoMetal for iOS 26,
  following dada's `gpu2/` and IOS-26-Hot's `phone/metal2`.
- Before committing, measure once on lisa: the same CoreAnimation scenes through the new stack versus today's
  driver, with the T733 GMEM patches applied. HakiOS's 50 vs 101 fps result says the Vulkan route is not
  automatically faster on this GPU. The 95 to 110 fps reports are on bigger GPUs.
- Keep our AIR to NIR to ir3 path in the tree until the new stack wins that measurement.
- metal2vulkan's licence (GPL-2.0+ or LGPL-3.0+) means a distributed `HakiosMetal.bundle` that links it must ship
  its source. Everything else in the stack is MIT/BSD, except adreno6xx's GPL-2.0-only Qualcomm-derived files.

## Not confirmed

- The fps and "working" claims above come from each project's README or docs. I did not reproduce any of them.
- Whether the T733 desktop is fast or merely correct: no fps published.
- Whether Turnip on a6xx gen4 exposes rasterization-order attachment access, which metal2vulkan needs for
  `[[color(n)]]` reads. Check with a capability dump on lisa.
- xVanTuring's Adreno650/610 and RigoLigoRLC's AdrenoMetal fork were not diffed against upstream.
- Social channels (X, Reddit, 4pda, XDA, Telegram) returned nothing specific.
