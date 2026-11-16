# iOS 27 on lisa: what UniXNU's 27 target really is, what moving would cost, and a recommendation

Research of 2026-11-16, host only. Nothing was run on the phone and nothing was downloaded beyond API answers.
The question was: "UniXNU has iOS 27 targets with the GPU stack ready, so why not go to 27 instead of rewriting
shaders?" The sources are the UniXNU checkout at `81e6460` (2026-11-17) and `research/UniXNU` at `5dc00d2`, the
AdrenoMetal checkout at `9820d00`, the prior-art clones listed in [gpu-prior-art.md](gpu-prior-art.md), the
ipsw.me API, Apple's `apple-oss-distributions/xnu` tags and the `ipsw-diffs/ios-27` diff catalog. Each claim names
its file.

## Short answer

- **UniXNU's iOS 27 is not a newer kernel.** It is the same `xnu-12377.121.6` we build today, with three extra
  patches. Apple has not published the Darwin 26 source. Apple's own 27.0.1 kernel is `xnu-13432.2.10`.
- **UniXNU's 27 target needs a CPU with pointer authentication.** Its docs say so in as many words, and every 27
  device in its catalog has PAC. Nobody has published iOS 27 on a CPU without PAC, or on an SM7325.
- **The GPU stack is not tied to 27.** AdrenoMetal + metal2vulkan + Turnip already runs iOS 26.5.2 on a CPU without
  PAC (IOS-26-Hot, Adreno 640) and iPadOS 17 on our exact GPU (Galaxy Tab S7 FE, A642L). What removes our shader
  work is that stack, not the OS version.
- **Moving to 27 does not remove work we have now; it adds some.** Our port does not use the parts of UniXNU that are
  "ready" for 27 (Apple's kexts, the arm64e kernel). Everything we wrote against 23F84's binaries has to be checked
  again on 27.
- **Recommendation:** stay on 26 and bring in the UniXNU GPU stack there first (kernel: NativeAdreno ABI v1 from
  RigoLigoRLC/adreno6xx; user space: Turnip-darwin + AdrenoMetal). That work carries over to 27 unchanged. Run the
  27 host checks (section 4, step M0) in parallel. Move to 27 only after the GPU is settled on 26, so that each
  step changes one thing.

## 1. What UniXNU's iOS 27 target is

### Kernel: compiled from source, and the source is xnu-12377

`profiles/os/ipados-27.0.1-24A446/profile.toml`:

```toml
"darwin" = "25.5.0"
# No Darwin 26 XNU source is published yet. 24A446 runs on the xnu-12377 kernel with the Darwin 26
# system calls added (patches/xnu/series); move to the Darwin 26 tag when Apple publishes it.
"xnu_version" = "12377.121.6"
"xnu_revision" = "ac9718fb1af618d5ce8678d0dc6e8a58f252216f"
```

That is the same tag and commit as the 26 profile (`profiles/os/ipados-26.5.2-23F84/profile.toml`) and as our
`xnu-work`. The difference is the patch series (`profiles/os/ipados-27.0.1-24A446/patches/xnu/series`):

| Patch | Lines | What | Do we need it |
| --- | ---: | --- | --- |
| `darwin26-syscalls.patch` | 476 | Eight syscalls in slots that were `nosys`: `pipe2` (148), `dup3` (149), `fchflagsat` (214), `getumask` (215), `aio_readv`/`aio_writev` (246/247, they answer `ENOTSUP`), `fsctlat` (249), `guarded_ftruncate_np` (352) | Yes. 27's libsystem_kernel calls them; our tree has none (`grep pipe2 bsd/kern/syscalls.master` in our xnu-work: nothing) |
| `darwin26-shared-region.patch` | 29 | Caps the shared region slide so a Darwin 26 cache, which "fills almost all of this kernel's arm64 shared region", still maps | Yes, checked against our 16 KiB layout. The 26 cache ends at `0x2b304c000`, 1232 MiB below the region end (`0x300000000`); 27's is larger |
| `darwin26-kpi.patch` | 549 | Exports Apple's 27 kexts link against (codesigning cdhash helpers, trust caches, AMFI, image4, MACF vfs hooks) | Mostly no: we load no Apple kext. Its codesigning and trust-cache parts may matter to our miniamfi; check during M1 |
| `zzz-boot-progress.patch` | 380 | Boot progress | Already in the 26 series |

`CONFIG_XNU_NATIVE_CS_HASH_CACHE=y` (27 config) belongs to UniXNU's 4 KiB code-signing hash cache patches. We run
16 KiB pages.

Apple's own kernel for 27.0 and 27.0.1 is `13432.2.10~2`, built 13 Aug 2026 (`ipsw-diffs/ios-27`,
`diffs/27_0_24A437_vs_27_0_1_24A446/README.md`). The newest public XNU tag is still `xnu-12377.121.6`
(`git ls-remote --tags https://github.com/apple-oss-distributions/xnu`, 2026-11-16). The macOS 27.3 profile says
the same: "Apple's own kernel of this release is xnu-13432.60.57".

The kexts are another matter. UniXNU's 27 kernel is prelinked with Apple's arm64e kexts from the 24A446
kernelcache (`kernelcache_sha256 38e6d9fa…`, 31 roots: AMFI, APFS, sandbox, IOSurface, IOMobileGraphicsFamily,
IOGPUFamily, IOHIDFamily…). `abi/original-classes.json` records the binary layout of 24A446's
`IOMobileFramebufferLegacy` (379 vtable slots, field offsets), `IOSurface` accessors and `IOHIDDevice`. The display
and GPU drivers hook those Apple classes.

### Reference IPSW and device

The 27 profile pins the iPad build:

| | Value |
| --- | --- |
| Product | `iPad17,3` (iPad Pro 13-inch M5, Wi-Fi), `j820ap`, chip-id 33090, board-id 12 |
| Build | 27.0.1 `24A446` |
| URL | `https://updates.cdn-apple.com/2026FallFCS/6074e110-d48d-40c2-b0e8-b611520941a3/iPad17,1,iPad17,2,iPad17,3,iPad17,4_27.0.1_24A446_Restore.ipsw` |
| Size | 11,654,176,243 bytes (10.85 GiB), SHA-1 `efe9b9f87a7bf373ceb309996614597bbd3f256b` (ipsw.me) |

It is the same IPSW family we use for 26 (`iPad17,1-4 26.5.2 23F84`). Newer builds exist (ipsw.me, 2026-11-16):

| Build | iPad17,3 | iPhone14,4 (our D16 identity) |
| --- | --- | --- |
| 27.0.1 24A446 (not signed) | 11,654,176,243 B, link above | 11,170,312,335 B, `.../2026FallFCS/0748752a-4554-47fe-b07e-36b5702830b8/iPhone14,4_27.0.1_24A446_Restore.ipsw` |
| 27.2.1 24B156 (signed) | 11,870,981,675 B, `.../2026FallFCS/02c18e38-cadb-4d4e-a754-96c00d73e8eb/iPad17,1,iPad17,2,iPad17,3,iPad17,4_27.2.1_24B156_Restore.ipsw` | 11,440,848,869 B, `.../2026FallFCS/e96b8dce-6bb4-48f0-b756-fb8cf31366e1/iPhone14,4_27.2.1_24B156_Restore.ipsw` |

### PAC: hardware required

`docs/porting/arm64e.md` (UniXNU):

> arm64e needs native PAC hardware: a CPU that implements pointer authentication and a hypervisor that leaves it
> usable [...] A device without it stays on arm64: iOS and iPadOS 18 are the last releases that support arm64, and
> iOS and iPadOS 27 are arm64e only.

The requirement table asks for FEAT_PAuth on every CPU (`ID_AA64ISAR1_EL1` APA/API and GPA/GPI non-zero) and key
registers writable at EL1. The SM7325's Cortex-A78 and A55 are ARMv8.2: none of that. The 27 profile sets
`"arch" = "arm64e"`, which selects `profiles/arch/arm64e` (PAC key programming, arm64e kexts). UniXNU's arm64
profile only goes with OS profiles up to 18.

Two consequences for us:

1. UniXNU's 27 build cannot be used as is on lisa. Its kernel signs and authenticates pointers in hardware, and the
   Apple kexts it links are arm64e.
2. 27 is not harder than 26 for PAC. iPadOS 26 is arm64e only too (our [ROADMAP.md](../ROADMAP.md): "the IPSWs no
   longer carry a plain arm64 dyld shared cache"). Our answer for 26, the PAC-rewriting pager plus the EL0 emulator
   ([pac-pager.md](pac-pager.md), [phase2-emulation.md](phase2-emulation.md)), does not depend on the version.

The oldest CPU iOS 27 supports is still the A13: iPhone 11 (`iPhone12,1`) and iPad 9 (`iPad12,1`) get 27.2.1, the
iPad 8 (`iPad11,6`, A12) stops at 26.7.3 (ipsw.me). Phase 1 found that 26's cache, built for the same A13 floor, needs
only `LDRAA`, `LDRAB` and `FJCVTZS` emulated ([phase1-featscan.md](phase1-featscan.md)). Expect the same on 27, but
confirm it with `featscan.py` on the real 27 cache before anything else.

### Ports of iOS 27 without PAC, or on SM7325

None found.

- UniXNU's `devices.toml` (397 records) lists 57 records on `ipados-27.0.1-24A446`. They are SM8550 (sheng and its
  forks, fuxi, vermeer, babylon), SM8635, SM8650, SM8850, SM7675, MT6897: all ARMv9 cores, which have PAC. The one
  exception is `garnet` (SM7435, Cortex-A78/A55, no PAC): "Preliminary garnet bring-up; not bootable".
- The arm64e doc's "Supported devices" table has only sheng (iPadOS 27 and macOS 27.3), "In development".
- GitHub search (2026-11-16, "iOS 27 xnu", "ios27", "ipados27", "iOS 27 port"): `obaosp/UniXNU-cupid` (SM8450, "does
  not boot yet"), `hannana-26/UniXNU-houji-ios27` (SM8650), `iLtanaka/UniXNU-marble` (SM7475, ARMv9),
  `hoshikv/peridot-xnu-build-ios27` (SM8635), `SadNutWall/arm64e-fuxi` (SM8550). All PAC hardware.
- SM7325 ports: `RigoLigoRLC/unixnu-sm-t733` is on `ipados-17.7.11-21H461` (arm64), `ETCHDEV/IOS-LISA` and this repo
  on 26.5.2.

The non-PAC 26 ports (IOS-26-Hot, ios26-sweet, ios26-sapphire, IOS-LISA, ours) have no 27 branch. IOS-26-Hot
mentions the iOS 27 SDK only as a build hazard (`HANDOFF.md:1155`).

## 2. What would change for us

Our port is not UniXNU's design. It is "Caminho B" ([kernel/apfs/README.md](../kernel/apfs/README.md)): a plain arm64
xnu-12377 with every driver built in, and **no Apple kext at all**. Our own APFS driver, `miniamfi` + `kern_minimac`
in place of AMFI and the sandbox, `MiniIOSurface`, `MiniIOMobileFramebuffer` and the `IOAcceleratorES` stand-in,
`MiniAppleKeyStore`, `hakios.apple80211.c`, `hakios.adreno.c` + `hakios.gpu.cpp`. So the parts of UniXNU's 27 target
that are "ready" (the prelinked Apple kexts and the classes in `original-classes.json`) are parts we do not use.

| Area | What changes on 27 | Evidence / where |
| --- | --- | --- |
| Kernel source | Nothing: still xnu-12377 | Section 1 |
| Kernel KPI for our drivers (display, ufs, touch, rtc, wifi, battery, adsp, sensors, camera, buttons, haptic, torch, bt, audio, cpufreq, scheduler) | Nothing. They are compiled into the same xnu-12377 tree with the same KPI. No Apple kext is linked, so `darwin26-kpi.patch` does not apply to them | `kernel/drivers/*`, `kernel/patches/apply.sh` |
| Kernel, user-facing | The eight syscalls, the shared region cap (redo for 16 KiB), maybe parts of the codesigning patch | `darwin26-*.patch` |
| Kernel, user-client ABIs our stand-ins emulate | Must be checked again. They were reverse-engineered from 23F84's frameworks: IOSurface create/lookup reply 0x88 bytes, plane 0x98, buffer 0xc68, `init` 0x28; IOMobileFramebuffer `SwapEnd` request 0x588 with 16.5's layout 0x48 further in. A change in 27's IOSurface.framework or IOMobileFramebuffer.framework breaks the display before the first frame. Not known yet | [kernel/drivers/display/README.md](../kernel/drivers/display/README.md) |
| Kernel, policy answers | `miniamfi` answers AMFI policy syscalls 0x5a, 0x60, 0x66 as 26's libraries ask them; `connport-wire.py` answers `mach_port_is_connection_for_service`. 27 may ask more. Unknown | [kernel/security/README.md](../kernel/security/README.md), [kernel/pg16k/README.md](../kernel/pg16k/README.md) |
| PAC pager, dyld pager, slide | Same code. UniXNU runs 27 on xnu-12377's own `vm_dyld_pager.c` and slide code with only the slide cap, so 27 uses no new chained-fixup or slide format. Our `slideauth-wire.py` and `dyldpager-wire.py` cover the formats 12377 knows | `profiles/os/ipados-27.0.1-24A446/patches/xnu/series` |
| Loader, U-Boot, vendor_boot | Nothing for the OS change. A new kernel image goes into vendor_boot as today | [flash-boot.md](flash-boot.md) |
| Root image | New IPSW, new sysroot, new `system-27.apfs`. `build-system-26.sh` decrypts AEA, 7z-extracts, lays out the cryptexes and needs about 35 GB of transient disk. The host has 29 GB free (`df`, 2026-11-16) with 9.8 GB in `ipsw/26.5.2`, 7.3 GB in `ipsw/26.5.2-extracted`, 5.8 GB in `ipsw/16.5`, 4.9 GB in `ipsw/sysos-symbols`: room has to be made first | `userland-26/README.md` |
| Selector, plist edits, daemon lists | `select-system-26.py` path rules (cryptex symlinks, metallibs, pruned bundles), `plist-tweaks.py` (`D16.Default.plist`, `ALWAYS_DISABLED`, `CRASH_LOOPING` labels), `lean-daemons.txt`: recheck every label and path | `userland-26/scripts/` |
| init26 | Raw syscalls and launchd's on-disk files (`disabled.plist`, keybags dir, persona setup). Likely portable; the `usermanagerd --init` and `keybagd` behaviour has to be seen again | `userland/init/` |
| libdlisa | Name-based, no fixed addresses (the only constant is the cache base `0x180000000` for unsliding backtraces). Every hook names a private class or selector that 27 may have renamed: `SBLockScreenManager`, `SBIdleTimerGlobalCoordinator`, `_UIStatusBarVisualProvider_Split` + `visualProviderSubclassForScreen:visualProviderInfo:`, `unlockUIFromSource:withOptions:`, `_finishUIUnlockFromSource:withOptions:`, `+[MCMUserIdentityCache personasAreSupported]`, `-[CALayerHost setRendersAsynchronously:]`, the `_state` ivar used by the lockdown fix, plus the PosterBoard, status bar and touch (virtual IOHID service) paths. Each needs a check, most should survive | `userland/inject/dlisa-inject.c`, `presentation-springboard.patch` |
| Our Metal plug-in (HakiosMetal) | `HakiosMetalCA-ios26.patch` carries 17 fixed 23F84 addresses; the Metal private ABI was diffed for 26 (Metal 373.2); 27's CoreAnimation and RenderBox shaders would go through our AIR to ir3 compiler again: the shader work the owner wants to stop | [kernel/drivers/gpu/README.md](../kernel/drivers/gpu/README.md) |
| Identity | D16 (iPhone 13 mini, `iPhone14,4`) gets iOS 27: 27.2.1 is signed today. We run the iPad cache under the D16 identity on 26 and can keep doing that. Keep the iPad build: AdrenoMetal's iOS 27 code has 24A446 iPad QuartzCore addresses (section 3) | ipsw.me, `loader/identity-soc.py` |
| Data volume (`darwin_var`, HFSX, 32 GiB) | A 26 volume holds 26's caches, registrations, keybags and the MobileGestalt cache. A real update runs the data migrator at first boot; that has never been tried on this port. Start 27 on a fresh volume (`tools/mkvarvol.py` + `gen-var-skel.py`), after archiving the 26 one the way `backups/lisa-planb-20261112/var-super-tail.hfsx.zst` was made, so going back to 26 is a restore | [data-volume.md](data-volume.md) |

## 3. The GPU on 27

### Does the UniXNU stack cover what CoreAnimation and Liquid Glass use

| Need | Covered? | Evidence |
| --- | --- | --- |
| Framebuffer fetch / `[[color(n)]]` reads | Yes on paper. metal2vulkan implements it with rasterization-order attachment access or pixel interlock and refuses it otherwise (`docs/REFUSALS.md:112`, "`FramebufferFetch`"). Turnip turns that feature on for every a6xx/a7xx: `tu_device.cc` sets `.EXT_rasterization_order_attachment_access = true` and `rasterizationOrderColorAttachmentAccess = true` with no condition (Mesa 25.0.7, `userland-26/gpu-build/mesa/mesa-25.0.7/src/freedreno/vulkan/tu_device.cc:314,709`). Our chip is in Turnip's table as "Adreno 7c+ Gen 3" (`chip_id=0x007506030500`, `common/freedreno_devices.py:700`). This settles the open item of gpu-prior-art.md, at least in source; a capability dump on lisa still has to confirm it | metal2vulkan `d9236a8`, Mesa 25.0.7 |
| Blur and backdrop compute | AdrenoMetal answers the completion-handler form of `newComputePipelineStateWithDescriptor:` that "iPadOS 27's QuartzCore builds its backdrop compute pipelines (Liquid Glass luma, backdrop capture) with"; without it the render server aborted at the first backdrop | `NativeBridgeMetalFrontend.mm:4898` |
| Function constants | AdrenoMetal: "Function constant dictionaries expose names, indices and types", specialised names kept. Whether metal2vulkan takes RenderBox's 128-bit `uint4 _shader_state` constant (which needed our own `wide-constants` patches on 26) is not confirmed | `docs/drivers/adreno-metal.md`, AdrenoMetal README |
| The 27 render server's framebuffer format | AdrenoMetal patches 24A446's QuartzCore in memory: `IOMFBDisplay::framebuffer_pixel_format` at `0x184461838`, the `CADeviceIsVirtualized` byte at `0x1e701bdd9` ("The stored option values and the framebuffer branch are addresses of iPadOS 27's QuartzCore (24A446)"). These are iPad 24A446 cache addresses: they hold for that build only | `NativeAdrenoMetalPlugin.mm:103-109,506-521` |
| Priming | 280 of 280 selected libraries primed on sheng's 27 image | `docs/devices/sheng.md`, GPU section |

### Reported performance

| Port | GPU | OS | Result |
| --- | --- | --- | --- |
| sheng (UniXNU) | A740 (SM8550), 120 Hz panel | iPadOS 27 24A446 | "fps average 72.1 and 68.2, low 41.5 and 35.2" (`metal-flow=0`); 67.4 and 73.2, low 30.5 and 32.3 (`metal-flow=1`). One panic at idle in the preemption IRQ, since handled by queuing preemption completion on a thread call. Pixel and frame-pacing qualification still listed as the next action |
| IOS-26-Hot | A640 (SM8150), no PAC, 60 Hz, 1080x2400 | iOS 26.5.2 23F84 | ~52.9 fps home swipe, ~39.7 fps Settings scroll (README) |
| HakiOS on redwood | A642L (our GPU) | iOS 16.5 | metal2vulkan + Turnip 50 to 57 fps page swipe vs 101 with hakios-metal (older pins, msm shim; see gpu-prior-art.md) |
| sm-t733 | A642L | iPadOS 17.7.11 | Desktop composited through Metal; no fps published |
| darwin-lisa today | A642L | iOS 26.5.2 | HakiosMetal: about 30 to 36 fps on the Settings scroll, clipping defects open ([gpu-device-test.md](gpu-device-test.md), sessions 7 to 9) |

Nobody has measured the A642L on 27. sheng's GPU is about three times ours; its 27 numbers do not transfer.

### What is OS-independent

- The kernel ABI. `NativeAdrenoABI.h` (adreno6xx) imports an IOSurface by pinning the caller's own pages
  (`NativeAdrenoParamsHostImport`: "BoImport pins the caller's own pages (an IOSurface's) as a BO"), so it works over
  our `MiniIOSurface` as well as over Apple's IOSurface kext.
- Metal discovery. AdrenoMetal is reached through `IOAcceleratorES` -> `MetalPluginName` -> bundle -> principal
  class -> `initWithAcceleratorPort:` (`NativeAdrenoMetalPlugin.mm:2`), the same protocol our stand-in already serves
  on 26 (gpu README, "Metal device discovery is the same protocol").
- Turnip with `-Dfreedreno-kmds=darwin` and metal2vulkan: the build differs only in target triple and deployment
  version (`scripts/build-turnip-darwin.py`, `build-adreno-metal-bridge.py`: `ios` = arm64 iOS 17, `ios27` = arm64e iOS
  27). On our pager-rewritten userland either an arm64 build relabelled arm64e (as our HakiosMetal today) or the
  upstream arm64e build (rewritten on page-in) should load.

So the GPU stack can be brought to 26 now, and the same kernel driver and most of the same user-space build go to 27
later. On 26 there is no upstream target: the ports carry their own changes (dada's `gpu2/patches`: about 10,400
lines over NativeMetal, metal2vulkan and Mesa, some for older pins; IOS-26-Hot's `phone/metal2`). On 27 AdrenoMetal
supports the iPad 24A446 build upstream. That is the one real argument for 27 on the GPU side.

## 4. The two paths

### Path A: migrate to 27 (iPad17,3 24A446 under the D16 identity)

| Step | Work | Done when (on the phone) | Estimate |
| --- | --- | --- | --- |
| M0, host | Make disk room. Download the 24A446 IPSW (11.65 GB). Extract the cache and the frameworks. `featscan.py` + `pacstrip.py scan` on the cache. Diff against 23F84: IOSurface.framework accessors, IOMobileFramebuffer swap request size, `class-dump --diff` of Metal, every class and selector libdlisa names, the AMFI policy syscall callers, LaunchDaemon labels | Host report: instructions to emulate, ABI breaks, renamed hooks | 1 day |
| M1, kernel + root | Apply `darwin26-syscalls` and `darwin26-shared-region` (re-derived for 16 KiB) to our tree, keep plain arm64 (do not take UniXNU's `--os 27`, it selects arm64e). Fork `select-system-26.py` and `build-system-26.sh` for 27, build `system-27.apfs`, fresh data volume | `init26` runs and Apple's launchd starts; no `SIGSYS`, no shared region map failure, no pager trap storm | 1 to 3 days |
| M2, home screen | Fix whatever M0 found in the stand-ins (IOSurface, IOMobileFramebuffer, miniamfi, connport), the CommCenter-style hangs, containermanagerd personas, lsd, the status bar and lock screen hooks | SpringBoard home on the panel, touch works, unlock works, software rendering | 2 to 7 days, the widest range: on 26 this stretch took most of the 2026-11-09 to 11-12 sessions |
| M3, GPU | Either port HakiosMetal again (17 addresses, Metal ABI, 27's shaders through our compiler) or bring in the UniXNU stack (same work as path B's G1 to G3, with upstream's ios27 plug-in) | backboardd publishes the Metal device, home composited by the GPU, fps measured with the same scenes as 26 | 3 to 8 days (B's G1 to G3 if not done before) |
| M4, peripherals | Kernel drivers unchanged. User-space bridges checked: bluetoothd bridge, the HAL plug-in for the speaker, AVFlashlight torch hook, brightness, sensors (HID), camera path, Wi-Fi (`hakios.apple80211.c` answers Apple80211 ioctls; wifid may have changed), battery properties | Each item's existing test line of [peripherals.md](peripherals.md) passes again | 2 to 4 days |

Total to reach today's 26 state, without any GPU gain: roughly 6 to 15 days. M0 can show it is cheaper (no ABI
breaks) or more expensive (a new instruction to emulate, a new IOSurface layout).

### Path B: stay on 26, bring in the UniXNU GPU stack

| Step | Work | Done when (on the phone) | Estimate |
| --- | --- | --- | --- |
| G1, kernel | NativeAdreno ABI v1 in our kernel: adreno6xx's user client on top of `hakios.adreno.c`, or adreno6xx itself with lisa's board inputs (same A660 firmware set, Yupik clocks, DVFS 315/450/550 MHz). Mind its GPL-2.0-only Qualcomm-derived files | Turnip's fill, compute and triangle tests exact on lisa (adreno6xx's 2026-10-25 milestone on the T733); `vulkaninfo`-style dump shows `rasterizationOrderColorAttachmentAccess` | 2 to 4 days |
| G2, user space | Turnip-darwin, the metal2vulkan bridge and AdrenoMetal for iOS 26 (dada's `build_turnip_ios.sh`, IOS-26-Hot's `phone/metal2`), arm64 relabelled arm64e like HakiosMetal | backboardd publishes the device, the home screen and Control Center composited, no refused pipelines in the log | 2 to 4 days |
| G3, measure and decide | GMEM and transient-attachment patches, priming list for 23F84's metallibs, then the same Settings scroll and home swipe as sessions 7 to 9, against HakiosMetal | 60 fps on the swipe and no clipping (the project's done criterion), or a measured reason why not | 2 to 3 days |

Total: roughly 6 to 11 days, and the result is the GPU the owner wants, on the userland we already know works. G1 is
reusable as is on 27; G2 becomes "build upstream's ios27 target" on 27.

## 5. Recommendation

**Path B now, then 27.** Reasons, in order:

1. The goal is to stop writing a shader compiler. That comes from the Turnip + metal2vulkan stack, and the stack runs
   on 26 without PAC today (IOS-26-Hot). Moving to 27 alone gives no GPU at all: we would be back to software
   rendering or to re-porting HakiosMetal.
2. "Tudo pronto" in UniXNU 27 means Apple's kexts on an arm64e kernel on a PAC CPU. We use neither, and cannot use
   the second. What we would really get from 27 is a fresh round of checks on every 23F84-specific piece we wrote.
3. Doing both at once puts two unknowns in one boot: a new userland and a new GPU stack. On 26 the GPU stack can be
   compared frame for frame with HakiosMetal on the same root.
4. The kernel stays xnu-12377 either way, so nothing on 26 is a dead end. Once B is done, A's M3 is mostly done too.

Start A's M0 in parallel: it is host-only and tells, for about a day of work, whether 27 is cheap (no new
instructions, unchanged IOSurface and framebuffer layouts) or not. Pick 24A446 (the build UniXNU and AdrenoMetal pin),
not 27.2.1, unless M0 shows a reason.

## 6. Not confirmed

- Whether 27's IOSurface.framework, IOMobileFramebuffer.framework and Metal private ABI changed against 23F84. Needs
  the IPSW (M0).
- Whether 27's cache needs more than LDRAA/LDRAB/FJCVTZS emulated. Same A13 floor, so probably not; `featscan.py`
  decides.
- How large 27's cache is under our 16 KiB shared region. UniXNU only says it "fills almost all" of the region.
- Whether Turnip at UniXNU's Mesa pin `c823a83e` still exposes raster-order attachment access unconditionally (checked
  in 25.0.7 only), and whether it really works on the A642L.
- The A642L with the UniXNU stack on any 26 or 27 frame-rate test: nobody has published one.
- What 27's first boot does with a 26 data volume.
- Day estimates are judgement from how long the 26 bring-up took, not measurements.
