# Handover 2026-11-16: the UniXNU GPU stack for lisa, T0 to T1 (host only)

Nothing was written to the phone. The pmOS runs of T0 were another worker's (numbers below). Details, tables and the
run lines: [gpu-driver-rewrite.md](gpu-driver-rewrite.md) sections 7 and 9.

## Built, NOT written

| Piece | sha256 |
| --- | --- |
| root **int4** `userland-26/out/system-26.apfs.full-a7d9c8b9` (9a91ed47 + libdlisa c8b291b8 + libdlisa-motion 677f8faf + `AdrenoMetal.bundle`) | `a7d9c8b9f792189c5ff0fbb5cd2e8db960fc04c13ab80666928a5ef0ac070df2` |
| root **int5** (deleted 2026-11-16, superseded) `userland-26/out/system-26.apfs.full-c93a8ec8`: int4 with libdlisa `fec154a7` (HEAD 37e0720: c8b291b8's bar and text probe + the virtual-BT fix; `out/libdlisa-37e0720`, build-inject.sh's recipe, which rebuilds c8b291b8 byte for byte from cca541f; run-tests.sh 11/0). Read back, census 61218 files, 0 bad | `c93a8ec87125465f8fc645a29cafc3fb7b53d8b642ee9c8c75ec09b1f1ab621f` |
| root **int6** (deleted 2026-11-16, superseded) `userland-26/out/system-26.apfs.full-2b1bf7d9`: int5 with `AdrenoMetal` `99f726b3`: one device per process (T1-A on int5 aborted in `MTLCreateSystemDefaultDevice`, "more than one device": our kernel publishes two IOAcceleratorES and the plug-in made a device for each; now the second port gets nil, as HakiosMetal does), and answers for the six private properties the runtime leaves to Metal (gpuCoreCount 2, maxVertexAmplificationCount 2, maxComputeThreadgroupMemory 32768, isLargeMRTSupported / supportsComputeCompressedTextureWrite / supportsConditionalLoadStore no). Read back, census 61218 files, 0 bad | `2b1bf7d9dd3858e4666af4047f02806b138ba2173035809b76d8a0cdffd15bdc` |
| root **int7** (deleted 2026-11-16, superseded) `userland-26/out/system-26.apfs.full-529954dc`: int6 with `AdrenoMetal` `6947cea0`: QuartzCore's default.metallib opened as a binary archive gets an empty archive (every lookup misses, pipelines from the library) instead of the error that was every process's first problem. Built on int6 as BASE (9a91ed47 is deleted; the same files). Read back, census 61218 files, 0 bad | `529954dcf3b80ca1cd7757ac9a1cf2f3d55b112422f84e816a16876b7f5027a4` |
| root **int8** (deleted 2026-11-16, superseded) `userland-26/out/system-26.apfs.full-e3259e0e`: int7 with `AdrenoMetal` `c5ddd1e9` (boot-args `tu_bb=`, `tu_cl=`). Read back, census 61218 files, 0 bad | `e3259e0e9814155a84db5dc6601bf104029a4854e31766edc0068ebc1ed2d802` |
| kernel `research/hackios-xnu-kernel/kernel-16k-unixnu1.bak`: the shared tree (periph2) + HakiosGPU's submit honouring GPU_SUBMIT_BO_TRACK + boot-arg `gpu_hwcg=1` (`build-kernel.sh`) | `35bc54e779d5f1fbddc1f8d87851aa49d91c16ae9971c6a7a867e119d7c6c51b` |
| root **int9** `userland-26/out/system-26.apfs.full-58bd61cd`: int8 with `AdrenoMetal` `87a66399` (pipelines no longer refused on a binary-archive miss). Read back, census 61218 files, 0 bad | `58bd61cde4258577ce3615278d0d7a6071c2a89578177ea05686960269006d36` |
| root **int10** `userland-26/out/system-26.apfs.full-cd5a49c4`: int9 with `AdrenoMetal` `6aca30f8` (private MTLResourceOptions bits dropped, not refused). Read back, census 61218 files, 0 bad | `cd5a49c409178c7ffb3d5f51b9208e22a947129ca2ebf98036d9b5457f19623e` |
| kernel `research/hackios-xnu-kernel/kernel-16k-unixnu2.bak`: unixnu1 + display-wire (one display start) + boot-arg `fb_settle_us=N` (the swap waits for a frame's submission that came after it) | `9dbfde4ff9dcfd983ddb38b17721ad7379dc0af94df17c8f65a47c6e7aea8e71` |
| kernel `research/hackios-xnu-kernel/kernel-16k-unixnu3.bak`: unixnu2 with Linux's clock gating ON by default (`gpu_hwcg=0` leaves it out); `fb_settle_us` off by default | `b802850f4ab8181af95019ab00adbf7451d2589f9d9e4f25d55c96363f4e987c` |
| root **int11** `userland-26/out/system-26.apfs.full-938a5722`: int10 with `AdrenoMetal` `7c55b1a8` (boot-arg `m2v_renderarea=1`; RGBA16Float_XR as RGBA16Float). Read back, census 61218 files, 0 bad | `938a5722d4adf003106dbbd40c50385b7601281329c58d125212ef403df30979` |
| root **int12** `userland-26/out/system-26.apfs.full-2bfc0622`: int11 with `AdrenoMetal` `ddb71374` + turnip `3ee69c77` (boot-args `tu_vsc=n`, `tu_condskip=0`; `vsc grew` in the log; private `getBytes` by blit). Read back, census 61218 files, 0 bad | `2bfc0622d938120caa5e6598b968eb4db38e173a58d6bb943229013f024832e5` |
| root **int13** (GPU hang in t6a, deleted) `userland-26/out/system-26.apfs.full-6b0860bb`: int12 with `AdrenoMetal` `a3b7f299` (RGB8_444_2P attachments fetched; a refused draw logs its bound textures). Read back, census 61218 files, 0 bad | `6b0860bb476268f9682f05e196fe9b122ea080d0a4dd1c792de251f4b13cc171` |
| root **int14** `userland-26/out/system-26.apfs.full-215d11f6`: int12 with `AdrenoMetal` `c592c0eb` + turnip `7d7559d1` (boot-arg `tu_sysload=n`; render-area and shadow copy-in counts; refused draws name their textures; no 542 fetch). Read back, census 61218 files, 0 bad | `215d11f6479b152964d2925d728b79249eaae51dde699f759dcbb89d431ad6db` |
| kernel `research/hackios-xnu-kernel/kernel-16k-unixnu4.bak`: unixnu3 + boot-arg `gpu_irq=1` (the GPU's interrupt wakes fences' waiters; SPI table in gic_vtimer.c) | `e7c15a5e5873d7fd320c234501c14d10a96d321ad3848d8d0e42642cbfa86603` |
| `AdrenoMetal.bundle/AdrenoMetal` (plug-in + metal2vulkan bridge, arm64e label; int14) | `c592c0eb97bee2fbcf44939e4a272ac4251a05728ee12fe9c2f90a30f3acac1c` |
| `AdrenoMetal.bundle/libvulkan_freedreno.dylib` (turnip, Darwin backend over HakiosGPU; int14) | `7d7559d1b32554c5130dee8a20d80054ea9e213b9e7783e5d83c15da9d62af55` |
| `userland-26/out/tu-capass/tu-capass` (T0, fixed, with `fetch1`/`fetchbar`) | `5535d43eacfcd36b57f4594830e52f60ec8abb47e04efea88a7b5edf7d10040a` |

Deleted on the coordinator's word: roots b915770a (first T1 root) and 5c001ebc (its motion library is in int4).

## Results

| Step | Result |
| --- | --- |
| T0 (pmOS, 550 MHz) | passed after the measurement fix (the first tu-capass never wrote its vertex buffer): home 0.92 ms (ours 5.6-7.0), main 2.61 (ours 6.3-6.9), fetch 7.64 = 2.9x main, but fetch1 (only the reading layer flagged, as metal2vulkan does) 1.26x; the barrier road (fetchbar) no better |
| T0b (host) | 251 of 251 pipelines CoreAnimation linked on lisa translate and compile to ir3 (all 79/126 uber-shader sets, glass, blur); QuartzCore 37/37 vertex, 126/128 fragment (not `inplace_copy`: pixel interlock), 11/24 kernels (the 13 `tile_*` are Apple tile shading); RenderBox `custom_fn` needs the caller's linked function at run time |
| T1 (host) | turnip, bridge and plug-in built on clang 22 + ld64.lld, root int4 built and read back |
| Kernel | no change needed; interrupts for fence waits are the first change if T2 shows the fence path holds frames |

## Next

T1-A (first frame, `m2v_clients=metal_test,backboardd`), then T2-A/B/C side by side, lines in section 7 (all at or
under 460 characters). Selection: `metal=1 metal_plugin=AdrenoMetal`; without `metal_plugin` the same root boots
today's HakiosMetal 9e29ff9c. Last resort for the glass, owner's decision, not implemented: Reduce Transparency by a
libdlisa boot-arg, only after measuring with it on.

## Toolchain added to the host

rustup 1.29.3 with Rust 1.98.1 and the `aarch64-apple-ios` target (owner's authorisation); glslang 16.0.0 built from
source into `userland-26/gpu-build/unixnu/tools/` (no cmake on this host); libdrm 2.4.134 headers for the host turnip.
