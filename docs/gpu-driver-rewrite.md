# GPU driver rewrite: which road, and the first test on lisa

Study of 2026-11-16, host only (nothing was run on the phone). Question from the owner: "isn't it better to redo our
driver in a more optimised way?" During the study the owner decided the current core will be rewritten, and is now
weighing a move to iOS 27 (UniXNU's target, with the GPU stack ready; `docs/ios27-migration.md`, another worker).
So this document answers: which base for the rewrite, what of it serves iOS 26 and 27 alike, and what is the
least-effort way to a working GPU without writing a shader translator of our own. Prior art is in
`docs/gpu-prior-art.md` (commit c18a592); this document does not repeat it.

## Short answer

1. **Do not fix the current core.** Every lever it lacks (binning with correct visibility streams, a sysmem/GMEM
   choice per pass, UBWC without a copy per buffer, raster-order framebuffer fetch, cheap draw recording) is code
   turnip already has, tested against the conformance suite, on this exact chip (Mesa's A635 row, a6xx gen4).
2. **Least effort, no translator of ours: the UniXNU userland stack** (turnip with the `darwin` kernel backend,
   AdrenoMetal + metal2vulkan) **over the kernel interface we already have.** dada (iOS 26, SM8750) runs exactly that
   over a HakiOS-lineage kernel that speaks msm DRM by IOKit selector, as ours does: `tu_knl_darwin_redwood.h`
   (594 lines, `gpu2/patches/mesa-85fda3b-dada.patch:2333-2930`) carries NativeAdreno ABI v1 over GEM_NEW, GEM_INFO,
   GEM_SUBMIT, WAIT_FENCE and GET_PARAM, with selector 0x7e for IOSurfaces and 0x7c for host pages (our 0x7d, same
   structure). Its pieces are OS-independent except the Metal plug-in's private-ABI layer, which exists for iPadOS
   17 and 27 upstream and for iOS 26.5.2 in two ports (IOS-26-Hot on an A640 without PAC, dada). The native kernel
   side for our chip exists too (`RigoLigoRLC/unixnu-adreno6xx`) for later.
3. **It is not proven fast on a6xx, so what decides is one measurement on lisa, and its first part needs no iOS
   work** (section 7, T0): our prototype `tu-capass` runs CoreAnimation-shaped passes through pmOS's turnip on the
   real A642L and prints the GPU time; the same scenes cost our core 5.6-7.0 ms (SpringBoard's home pass) and
   6.3-6.9 ms (the Settings main pass) on iOS 26. The a6xx records of the UniXNU stack are poor: HakiOS 50-57 fps on
   16.5 (this chip), T733 a 33.7 ms median frame interval (this chip at 315 MHz, iPadOS 17), IOS-26-Hot 52.9 presents
   a second on a 60 Hz panel with live backdrop blur switched off (A640). Then T1/T2 put the stack on iOS 26 beside
   today's driver (boot-arg selected) for the fps of the same gestures, Liquid Glass blur on.
4. Keep our AIR to NIR front end as the fallback translator, not the plan: Mesa's runtime lets NIR go straight into
   turnip (`VkPipelineShaderStageNirCreateInfoMESA`), so the hybrid exists, but it costs a binding-model lowering and
   a Metal object layer we would write ourselves (section 4, option B2).

## 1. Why the current core is slow, in its own numbers

| What | Our core (iOS 26) | Mesa on the same chip | Source |
| --- | --- | --- | --- |
| Full-screen blended draw, 1080x2400 | 0.91 ms, linear sysmem target | 0.38 ms freedreno (UBWC), 1.45 ms tiled uncompressed | gpu-device-test.md Session 13 (glfill on pmOS, 550 MHz) |
| UBWC target | slower: backboardd's GPU 0.59 to 7.4 to 17.6 ms a buffer with `metal_ubwc_fb`/`metal_ubwc_rt`, and wrong pictures (a 2D-engine copy per buffer at commit) | UBWC is the default for every attachment and sampled image; no copy | owner's runs; Session 13 glass10/11 |
| GMEM | binning leaves tiles of the frame before (gpu-p2, gpu-v6); 76 bins of 544x64; without binning every draw runs in every tile (16 ms for 85 draws); now off by default | one pass, 20 bins of 544x240 for a 4-byte target, binning on (prototype below) | Session 6 |
| Choice sysmem/GMEM | our `preferGmem` per kind of pass, binning off; "no better than sysmem" (gpu-p3) | autotune per render pass instance from the GPU's own sample counts (`tu_autotune.cc:541`) | Session 6; tu_autotune.cc |
| CPU per draw | 55-110 us of the core per draw on the phone, 7-9 ms for an 85-draw frame | 0.12-0.48 us per draw on a Ryzen 7 5800X (prototype); even 20 times slower on an A78/A55 it stays under 10 us | Session 6; section 6 |
| Framebuffer fetch | an in-pass barrier, or a pass of its own when a target is compressed (`hakios-metal-y-ubwc-fetch.patch`) | input attachment with `SINGLE_PRIM_MODE` FLUSH_PER_OVERLAP in GMEM, FLUSH_PER_OVERLAP_AND_OVERWRITE in sysmem; autotune keeps such a pass in GMEM | tu_pipeline.cc:3351-3380, 3969-3976; tu_autotune.cc:553 |
| Unbound buffers | zeros pushed for a constant range of an unbound buffer (~300 a boot: `fixed_vert_lph_spc` buffer 3, `fixed_frag_lph_cph` buffer 2), the cause of the square corners | not a turnip question: Metal's argument tables are the layer above. metal2vulkan also reads zeros from a slot never bound, but keeps every binding as Metal state for the encoder's life: if CA relies on a binding made earlier in the encoder, it is honoured; if the buffer is never bound, the fix is in what CA expects there (section 5) | coordinator, Session 15 |

The pmOS UI at 90 Hz is freedreno doing the 0.38 ms column. Our core cannot reach that column without writing what
turnip already is.

## 2. The options

| | Kernel interface | Command layer | Shader translator | Metal object layer | Weeks (honest) | Main risk |
| --- | --- | --- | --- | --- | --- | --- |
| **A. Fix our core** | ours (msm-like) | ours (render.cpp, 3353 lines + 36 KB of UBWC patch) | ours (AIR to NIR to ir3) | ours (HakiosMetalCA.m, 8747 lines + 17 patches) | 10-16 for binning, autotune, UBWC in place, CPU cost; LRZ is useless for CA (no depth) | re-deriving turnip one bug at a time, on the phone |
| **B1. UniXNU userland over our kernel (dada's road)** | ours (msm by selector), carried by dada's `tu_knl_darwin_redwood.h` | turnip (Mesa main as dada pins it, `85fda3b`) + `tu_knl_darwin.cc` | metal2vulkan (AIR to SPIR-V) | AdrenoMetal with dada's / IOS-26-Hot's iOS 26 changes | 2-4 to a composited iOS 26 lock screen on lisa | a6xx records are 30-57 fps; live backdrop blur off in IOS-26-Hot; metal2vulkan GPL/LGPL; Rust toolchain for iOS (below) |
| **B1n. B1 with the native kernel ABI** | NativeAdreno ABI v1 + adreno6xx's async extension (graft or adopt `unixnu-adreno6xx`) | turnip + `tu_knl_darwin.cc` + `turnip-darwin-async-submit.patch` (618 lines, from the T733 tree) | metal2vulkan | AdrenoMetal | +3-5 over B1 (3-5k kernel lines grafted) | GPL-2.0-only A6xx files in an APSL kernel; selector 17 means different things in Adreno740 and adreno6xx |
| **B1'. HakiOS's 16.5 road** | ours (turnip's **msm** backend + `hakios-drm`) | turnip 25.0.7 msm | metal2vulkan `624bab3` | AdrenoMetal (old) | 2-4 | the setup that measured 50-57 fps: user-space syncobjs wait on the CPU before every submit, compute merge into the IOSurface. Superseded by B1 |
| **B2. Our plug-in over turnip, our AIR to NIR** | either | turnip | ours, NIR handed to turnip by `VkPipelineShaderStageNirCreateInfoMESA` (`vk_pipeline.c:69-91`) | ours, rewritten over Vulkan | 8-14 | we write the Metal-to-Vulkan binding model and the whole object layer; the front end must live in the same binary as turnip's NIR (one copy of `glsl_type`s) |
| **C. Gallium freedreno + Metal over Gallium** | ours (freedreno speaks msm via `hakios-drm` today) | freedreno Gallium (GMEM, UBWC, autotune "fd_gmem" heuristics) | ours (AIR to NIR, freedreno takes NIR) | ours, over `pipe_context` (Chlorophyll did this on A630) | 8-12 | Gallium's state model fights Metal's (argument tables, fetch, memoryless); no community on iOS uses it |

"Weeks" assume one person at this project's pace and count only the GPU, not the iOS 27 migration.

Why B1 is the least effort: of the five layers it writes none. The kernel stays as it is (dada's adapter needs our
service name and 0x7d for its 0x7c), the command layer is turnip, the translator and object layer are metal2vulkan and
AdrenoMetal, and the plug-in's iOS 26 layer exists in dada and IOS-26-Hot. B1n is the same userland with a better
kernel, a step to take after B1 runs, if T2 shows the fence path is what holds frames. B2 and C keep our translator
but force us to write a Metal object layer, which is the half of the work the owner wants to stop doing.

Toolchain note for B1: metal2vulkan is Rust (`rust-version = 1.98`, a pinned nightly in `rust-toolchain.toml`). The
host has Fedora's rustc 1.98.1 but no rustup and no `aarch64-apple-ios` standard library, so building the bridge for
iOS needs that target installed (a download, for the owner to allow) or `-Zbuild-std` on a nightly. dada builds with
Xcode on macOS; its scripts (`gpu2/build_turnip_ios.sh`, `build_bridge.sh`, `build_plugin.sh`) are the recipe to port
to our clang 22 + ld64.lld host, which section 6 shows builds turnip for iOS.

## 3. Kernel interface: our msm emulation against NativeAdreno ABI v1

**What we have.** `kernel/drivers/gpu/hakios.gpu.cpp` answers the msm DRM requests by number (GET_PARAM, GEM_NEW,
GEM_INFO, GEM_CPU_PREP, GEM_SUBMIT, WAIT_FENCE, SUBMITQUEUE_*, GEM_CLOSE, plus 0x7d user memory and 0x7e IOSurface);
`hakios.adreno.c:3260-3289` answers the parameters turnip asks (GMEM_SIZE, CHIP_ID with speed bin, TIMESTAMP,
PRIORITIES, HIGHEST_BANK_BIT, UBWC_SWIZZLE, MACROTILE_MODE). Turnip's msm backend already ran over it on 16.5
(`research/hackios-xnu-kernel/tools/mesa/turnip-darwin.patch`, 432 lines, and `hakios-drm/hakios.drm.c`, 645 lines),
and it still builds: section 6. Its limits, read in the code:

- no interrupts: "waiting is asking again five times a millisecond" (hakios.gpu.cpp header);
- one global lock, and `GEM_CPU_PREP` waits for all the GPU's work, not the buffer's (hakios.gpu.cpp:1266-1278);
- the submit is synchronous to the ring; no job queue, no kernel-side dependencies;
- syncobjs live in user space: `submit_with_syncobjs` waits on the CPU for every in-syncobj before it submits
  (hakios.drm.c:537-546), which serialises the CPU behind the GPU on every frame that has a semaphore.

**What NativeAdreno ABI v1 gives, on our chip.** `RigoLigoRLC/unixnu-adreno6xx` (18,965 lines; user client, submit,
scheduler, VM, memory, ring and IRQ are 7,050 of them): the GPU interrupt retires jobs and wakes waiters; an async
queue of 64 jobs, 8 on the ring; timeline points and host signals in the kernel; "a producer already issued on this
ring satisfies a GPU scheduling dependency"; display reads tracked per BO; coherent GPU mappings (WB/WA) for ordinary
BOs; DVFS 315/450/550 MHz with paired DDR votes; per-job CP timestamps for profiling (README). Turnip's side is
`tu_knl_darwin.cc` from `patches/mesa/turnip-darwin-kmd.patch` (78 KB), which also fills turnip's UBWC configuration
from the driver's DeviceInfo.

Note that the base `tu_knl_darwin.cc` is no better on synchronisation: it uses only ABI v1's synchronous calls, keeps
binary and timeline syncs in user space with condition variables, polls wait-any with `usleep(200)`
(`turnip-darwin-kmd.patch:1536`) and meets a full ring by waiting for the oldest fence. Only adreno6xx's async
extension plus the T733 tree's `turnip-darwin-async-submit.patch` moves dependencies into the kernel.

**What dada needed of a msm-speaking kernel** (its adapter's header, and IOS-26-Hot `METAL-NEXT.md:43-46`), checked
against ours:

| Need | Ours |
| --- | --- |
| msm selectors 0x40+n, kernel-chosen IOVA (msm 1.7 softpin) | yes (`hakios.gpu.cpp`) |
| IOSurface import by ID, 0x7e | yes, same structure |
| import of the caller's pages, 0x7c `{address, bytes, handle, flags}` | yes as 0x7d `hakios_user_memory`, same layout: change the adapter's number |
| every imported surface named in each submission, so the display waits for the last writer ("drawn") | yes: `b->surface->drawn = request->fence` for every named BO (hakios.gpu.cpp:1113) |
| the fence written after a cache flush event (Linux's CACHE_FLUSH_TS) | yes: `CP_EVENT_WRITE` CACHE_FLUSH with address and fence (`hakios.adreno.c:3502-3508`) |
| waits woken by an interrupt | no: polled; the first kernel improvement after B1 runs |

**Choice.** Start on our kernel (B1). Move to NativeAdreno ABI v1 with the async extension (B1n) only when T2 shows the
fence path costs frames: (a) graft adreno6xx's user client, submit, scheduler and IRQ onto our `hakios.adreno.c`
bring-up, or (b) adopt adreno6xx with our board inputs. Licences: `NativeAdrenoA6xxGmu.{h,cpp}`,
`NativeAdrenoA6xxHwInit.cpp`, `NativeAdrenoA6xxPower.{h,cpp}`, `NativeAdrenoA6xxSmmu.{h,cpp}` are GPL-2.0-only
(Qualcomm tables from Samsung's kernel); the rest is MIT. Either way the kernel serves iOS 26 and 27 alike: the ABI is
between our kernel and turnip, Apple is not in it.

## 4. The hybrid (our AIR to NIR inside turnip), for the record

Mesa's common Vulkan runtime takes a NIR shader instead of SPIR-V when a stage carries
`VkPipelineShaderStageNirCreateInfoMESA` (sType 1000290001, `vk_pipeline.h:42-52`) and no module:
`get_builtin_nir()` validates and clones it (`vk_pipeline.c:69-91,141-152`). turnip's own meta shaders use it. So
our front end's NIR can reach ir3 through turnip with no SPIR-V. What it needs that we do not have:

- the front end in the same image as turnip (it passes a `nir_shader *`; two static copies of NIR disagree on
  `glsl_type` pointers);
- NIR in Vulkan's shape: descriptor set/binding variables, push constants, `subpassLoad` input attachments, instead of
  our core's slot numbers and `isaml` fetches: a lowering of Metal's buffer/texture/sampler indices to a pipeline
  layout, i.e. metal2vulkan's binding plans, written again;
- the whole Metal object layer over Vulkan (encoders, argument tables, setBytes, blit, compute, IOSurface import).

That is weeks of work to keep a translator that metal2vulkan already provides and that the iOS 26 ports measured
pixel-exact on Liquid Glass (dada). Worth it only if metal2vulkan's licence or coverage blocks us.

## 5. Liquid Glass coverage

What today's core does right is the bar: Liquid Glass with its blur, rounded tiles, compute (gpu-p6). Read from the
clones of `gpu-prior-art.md` (paths relative to them) and from section 6:

| Feature | Today's core | metal2vulkan + AdrenoMetal (+ iOS 26 ports) | On turnip / A642L |
| --- | --- | --- | --- |
| Function constants, incl. RenderBox's 128-bit `uint4 _shader_state` and `newFunctionWithDescriptor:` | yes, after `*-wide-constants.patch` | every MTLDataType incl. UInt4 serialised (`UniXNU_AdrenoMetal/NativeBridgeMetalFrontend.mm:1699-1705,1750-1790`); `newFunctionWithDescriptor:` with `specializedName` (`:1800-1813`); constants baked in the translator | n/a (SPIR-V specialised before turnip) |
| Framebuffer fetch `[[color(n)]]` (the uber shader, every draw) | in-pass barrier or a pass of its own | ordered via rasterization-order attachment access or interlock; else input attachments + per-draw self-dependency barriers (`steelbrain_metal2vulkan/docs/API.md:1133-1140`) | `rasterizationOrderColorAttachmentAccess` = 1 on the A635 (prototype); the pass stays in GMEM with FLUSH_PER_OVERLAP. GMEM BGRA input reads had red/blue swapped: `turnip-gmem-memory-side-swap.patch` |
| Live backdrop blur (CABackdropLayer: downsample, `narrow_blur`, `variable_blur`) | drawn (render pipelines; 8 of 24 CA kernels compile) | only `narrow_blur` (a render pipeline) tested (`UniXNU_AdrenoMetal/bridge/quartzcore_blur_host.mm:24-27`); **IOS-26-Hot switches live backdrop layers off** in backboardd and SpringBoard (`HANDOFF.md:18`); no record of CA's compute blur | ordinary passes (the `blur` scene) |
| RenderBox `custom_fn` (MTLLinkedFunctions) | refused functions fixed one by one | linked; without it "the glass those effects draw was missing" (dada `nativemetal-dada.patch:2109-2110`) | n/a |
| setBytes / buffers a draw leaves unbound (our square corners) | zeros pushed for an unbound buffer's range | bytes copied at the call (over 4096 refused, `docs/METAL-COVERAGE.md:1995,2002`); bindings persist as Metal state; an unbound buffer reads zeros (`crates/metal2vulkan-runtime/src/render.rs:12698-12704`), unbound textures/samplers get neutral ones | n/a |
| Memoryless attachments (CA's attachment 1, the clip mask) | ordinary memory, not cleared at DontCare (Session 15) | lazily allocated transient; real backing when fetched through interlock or when the pass splits (`API.md:2650-2656`); IOS-26-Hot's garbled glyphs were attachment 1 never cleared (`HANDOFF.md:551`) | `turnip-transient-attachments-in-gmem.patch`: the sysmem road cost 9-12 ms GPU, 32-112 MiB and up to 309 ms of page wiring |
| Drawing the display's IOSurface | in place, linear | IOS-26-Hot shadows it (copy in and out, ~1 ms a frame); in place came out in bands and was reverted (`HANDOFF.md:358-361`); upstream metal2vulkan shadows padded surfaces | linear colour attachment works and GMEM resolves straight into it (prototype); UniXNU's in-place GMEM road for unpadded surfaces: `turnip-render-mode-for-attachment-memory.patch:26-47` |
| Shader compile stalls | stage cache seeded into the image (`metal-cache-seed.sh`) | priming lists and pipeline packs; T733: 122 pipelines 11.6 s, worst 3.2 s; IOS-26-Hot recompiled unlinked RenderBox shaders every frame (40 ms) until fixed | turnip's disk cache (`-Dshader-cache=enabled` in dada) |

Open gaps recorded by the iOS 26 ports: chronod widgets white under Metal (cause open), `_RB4` / RGB8_444_2P,
chronod, WidgetRenderer and PosterBoard forced to the CPU path (IOS-26-Hot `HANDOFF.md:12,25,351-353`); on lisa's
own old path "vibrancy/CC glyphs, widgets, flicker" (ETCHDEV `STATUS-2026-11-03.md:9`). So B1 starts behind today's
core on the backdrop blur, and that is what T2 must check first.

Licences: metal2vulkan GPL-2.0-or-later OR LGPL-3.0-or-later (a shipped bundle that links it must offer its source);
AdrenoMetal, Adreno740 and the turnip patches MIT; UniXNU core BSD-2-Clause-Patent. darlinghq/indium (0BSD) is the
permissive alternative but has no recorded CoreAnimation coverage; it is a fallback for the licence, not for effort.

## 6. Prototype (host): turnip built for our target, and CoreAnimation's passes through it

Everything below is reproducible from `kernel/drivers/gpu/tools/turnip-proto/` (README section 8).

**Turnip for iOS arm64, from our own Mesa tree.** The Mesa 25.0.7 tree `mk-mesa-darwin.sh` patches for our compiler
already carries HakiOS's `turnip-darwin.patch` and `hakios-drm`. Configured with `-Dvulkan-drivers=freedreno
-Dfreedreno-kmds=msm` against the same cross file (clang 22 + ld64.lld, iPhoneOS SDK), it builds in 23 s on this host
(625 targets): `libvulkan_freedreno.dylib`, Mach-O arm64, 10.4 MB (9.9 MB stripped), exports `vk_icdGetInstanceProcAddr`
and `vkGetInstanceProcAddr`, imports `IOServiceOpen`/`IOConnectCallStructMethod`/`IOConnectMapMemory64` (the HakiosGPU
user client) from IOKit, libSystem, libz and libc++. One build fix: a meson re-run must keep `PKG_CONFIG_LIBDIR` at
the SDK's `.pc` files, or it links the host's `libelf`/`libudev`. No glslang on this host: a stub stands in for the
BVH (ray-tracing) shaders, so acceleration structures are unusable in this build (CA does not use them).

**Turnip on the host, deciding as on the phone.** The same tree built for x86_64 with Mesa's `drm-shim`
(`freedreno_noop`) and one row added for the Adreno 642L (`FD_GPU_ID=635`: chip `0x06030500`, 512 KB GMEM, as our
kernel reports): turnip names it "Adreno 7c+ Gen 3", Vulkan 1.3.305. Nothing executes, but turnip makes every
choice it makes on hardware and records every packet. `tu-capass` drives it with no Vulkan loader, CoreAnimation's
shapes (textured layer tinted by a push constant, premultiplied source-over), at 1080x2400:

| Scene | What turnip chose (`MESA_GPU_TRACES=print`) | CPU to record (median, Ryzen 7 5800X) |
| --- | --- | --- |
| main: 8 full-screen layers into the BGRA8 framebuffer, Load/Store | GMEM, binning on, 20 bins of 544x240, UBWC on, one load + one store per bin | 3.7 us, 0.46 us a draw |
| linear: the same into a LINEAR BGRA8 target (the IOSurface itself) | linear colour attachment and blend supported; GMEM, binning, 20 bins, resolved straight into the linear image (no compute merge) | 3.6 us |
| home: 1 wallpaper + 80 icon quads, 4 textures | GMEM, binning, 20 bins, UBWC | 11.0 us, 0.14 us a draw |
| fetch: 8 layers blending in the shader from `[[color(0)]]` (input attachment, raster order) | `rasterizationOrderColorAttachmentAccess` = 1 on the A635; GMEM, binning, 20 bins (autotune keeps raster-order passes out of sysmem) | 3.6 us |
| blur: 4 draws into a 540x1200 RGBA16F target (Clear/Store), then 8 layers sampling it | backdrop pass in sysmem (5 draws or fewer: `fallback_use_bypass`), UBWC on; main pass GMEM + binning | 5.8 us |

With `TU_DEBUG=sysmem` the main pass records in 1.9 us. A queue submit plus fence wait costs the driver 10-15 us of
CPU when autotune is on (it adds its result buffer), 0.5 us otherwise. The GPU column is zero under the shim; the
pmOS build of the same program (section 7) fills it on the phone.

What this settles: (1) turnip builds for our toolchain and target today; (2) on this chip it renders the
framebuffer pass in GMEM with working binning and UBWC, and can resolve straight into a linear IOSurface, which is
the copy that sank the 16.5 Vulkan run ("each frame renders to a tiled image and merges back into the IOSurface with
a compute pass", HakiOS `metal-measurements.md`); (3) raster-order framebuffer fetch, which metal2vulkan needs for
`[[color(n)]]`, is exposed on the A635 (this answers the open question of `gpu-prior-art.md`); (4) its CPU cost per
draw is two orders of magnitude under our core's.

What it does not settle: GPU time (needs hardware), autotune with real history (the shim returns no sample counts),
metal2vulkan's own CPU cost per Metal call (not built here; Rust 1.98 toolchain), and the 16.5 run's other losses
(user-space syncobjs, polled fences).

## 7. Plan, with marks checked on the phone

Each step leaves today's driver selectable (`metal_plugin=` boot-arg, gpu-wire.py step 6), so vidro, recortes and
compute that work today keep working until the new stack beats them.

**T0. Turnip's GPU time on lisa, under pmOS (1 run, no iOS build).**
`kernel/drivers/gpu/tools/turnip-proto/build-pmos.sh` builds `tu-capass` for aarch64 musl
(`userland-26/out/tu-capass/tu-capass`, sha256 `545c0413…` for this build; the temporary directory in its debug
information changes the hash from build to build. Checked under qemu-aarch64 against the lisa chroot up to device
enumeration). On pmOS, with the GPU at its top clock as for glfill:

```sh
for s in caps main linear home fetch blur; do ./tu-capass /usr/lib/libvulkan_freedreno.so $s 300; done
TU_DEBUG=sysmem ./tu-capass /usr/lib/libvulkan_freedreno.so main 300
MESA_GPU_TRACES=print ./tu-capass /usr/lib/libvulkan_freedreno.so home 5 | tail -40
```

Pass: `home` (81 draws, SpringBoard's shape) well under our core's 5.6-7.0 ms a pass (Session 6, sysmem) and `main`
(8 layers) well under 6.3-6.9 ms; `fetch` within ~1.5x of `main`. If turnip on the real chip is not clearly faster
than our core on the same scenes, stop: the problem is not the command layer.

**T0b (host, after T0).** metal2vulkan's translator over QuartzCore 26's `default.metallib` (the 189 functions our
front end was measured on: 173 translate, all 37 vertex and 128 fragment compile) and the SPIR-V through the host
turnip of section 6 (drm-shim, `FD_GPU_ID=635`) into ir3: the coverage count against ours, on the host, before any
boot. Needs cargo to fetch metal2vulkan's crates (a download, for the owner to allow).

**T1. The UniXNU userland on iOS 26, over today's kernel, beside today's driver (first frame).**
No kernel change: dada's `tu_knl_darwin_redwood.h` with `HakiosGPU` as the service and 0x7d for host pages. Userland:
turnip (`-Dfreedreno-kmds=darwin`, dada's Mesa `85fda3b` with its patch) and AdrenoMetal + metal2vulkan built as
dada's `gpu2/build_turnip_ios.sh` / `build_bridge.sh` / `build_plugin.sh` do, ported to our clang 22 + ld64.lld host,
with the iOS 26 changes of dada and IOS-26-Hot (`phone/metal2`), installed as a second bundle selected by
`metal_plugin=AdrenoMetal`. Marks: `metal_test` draws and reads back through it; backboardd composites the lock screen
through the new plug-in with no software fallback line.

**T2. The measurement that decides.** Same root, same gestures as Session 6/13 (lock to home swipe, Settings scroll,
Control Center opened from the home screen), two boots: today's plug-in and AdrenoMetal (turnip's autotune, then
UniXNU's `TU_DEBUG=gmem` default). Read `MiniIOMobileFramebuffer: from one frame to the next` (interval histogram)
and the GPU per command buffer. Pass: median frame interval clearly under today's (median 18-21 ms in the Settings
scroll, Session 6) and at or under 11.1 ms (90 Hz) in the swipe, with Liquid Glass right **and its live blur on**
(the coverage table of section 5; IOS-26-Hot reached its 52.9 fps with it off). Fail on fps with the GPU idle means
the CPU side (metal2vulkan or the fence path): profile with UniXNU's frame diagnostics, then consider B1n (interrupts,
async submit) before touching turnip.

**T3. Then remove the old core** (hakios-metal, its 17 patches and `HakiosMetalCA.m`), keeping the AIR to NIR
front end in the tree as the B2 fallback until T2 has passed on two roots.

**What serves iOS 26 and 27 alike.** The kernel and its interface (ours, or NativeAdreno later), turnip and its darwin
backend, metal2vulkan's translator and runtime, our prototype and T0/T0b: none of them touches Apple's ABI. What
differs is the plug-in's private-Metal layer (`_MTLDevice` subclass, selectors CoreAnimation sends, IOSurface and
IOAccelerator glue): for 27 it is UniXNU upstream's `--target ios27` build of AdrenoMetal; for 26 it is dada's and
IOS-26-Hot's changes on top of it. The arm64e label question is the same on both (our bundle is plain arm64 relabelled,
as today). T2's scenes are the same.

## 8. Files of this study

- `kernel/drivers/gpu/tools/turnip-proto/tu-capass.c`: the scenes above, through any turnip, no loader.
- `kernel/drivers/gpu/tools/turnip-proto/gen-spirv.py`: its three shaders as SPIR-V words (no glslang on the host).
- `kernel/drivers/gpu/tools/turnip-proto/mesa-host-shim.patch`: the host build's two edits to the Mesa tree (the
  A642L row in drm-shim; glibc 2.43's C23 `once_flag` against Mesa's c11 threads).
- `kernel/drivers/gpu/tools/turnip-proto/build-pmos.sh`: `tu-capass` for pmOS on lisa.

Host build, from a copy of `userland-26/gpu-build/mesa/mesa-25.0.7` with the patch applied (glslangValidator stub
on `PATH`, a `libdrm.pc` pointing at libdrm 2.4.134 headers and the host's `libdrm.so.2`):

```sh
meson setup host mesa --buildtype=release -Db_ndebug=true -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=freedreno \
  -Dfreedreno-kmds=msm -Dtools=drm-shim -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
  -Dgles2=disabled -Dllvm=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dvideo-codecs= -Dlmsensors=disabled -Dbuild-tests=false -Dperfetto=false -Dvulkan-layers=
ninja -C host src/freedreno/vulkan/libvulkan_freedreno.so src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so
python3 gen-spirv.py spirv.h && cc -O2 -I mesa/include -I . -o tu-capass tu-capass.c -ldl
LD_PRELOAD=host/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so FD_GPU_ID=635 MESA_GPU_TRACES=print \
  ./tu-capass host/src/freedreno/vulkan/libvulkan_freedreno.so main 1
```

The iOS build: the same tree, `--cross-file userland-26/gpu-build/mesa/cross.ini`, `PKG_CONFIG_LIBDIR` at its
`pkgconfig/`, `-Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dtools=` and the options above. Both builds were
deleted after the measurements (disk).
