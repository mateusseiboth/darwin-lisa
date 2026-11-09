# Roadmap: iOS 26 on lisa

Written 2026-11-07, from the public state of the projects below as read that day.

## The wall: pointer authentication

Since iOS 17 every system binary is **arm64e only**: iOS 17 dropped the A11, the last iPhone without pointer
authentication (PAC, ARMv8.3), so the IPSWs no longer carry a plain arm64 dyld shared cache. iOS 26 needs an A13 or later.

The SM7325's Cortex-A78 and Cortex-A55 are ARMv8.2 and **have no PAC**. The hint-space PAC instructions (`PACIBSP`,
`AUTIBSP`) run as `NOP` there, but the others (`RETAB`, `BLRAA`, `LDRAA`, `PACIA`, ...) are undefined and trap. An iOS 26
binary dies at its first `RETAB`.

## What exists

| Project | Kernel | User space | CPU | What it gives us |
| --- | --- | --- | --- | --- |
| [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) | xnu-8796 (iOS 16) | iOS 16.5, iPhone X (arm64) | **SM7325** (POCO X5 Pro 5G) | drivers for our SoC: display, touch, Adreno 642L (Mesa, Metal), Wi-Fi, audio, cameras |
| [UniXNU](https://github.com/UniXNU/UniXNU) | **xnu-12377 (iOS 26)** | **iPadOS 26.5.2**, also 17.7.11 and 27.0.1 | SM8550 (has PAC); an SM7325 tablet on iPadOS 17 | the iOS 26 kernel profile, a UEFI boot path (Mu-Silicium, which has `lisaPkg`), a Linux toolchain for XNU |
| [Bergamota](https://github.com/ry-diffusion/bergamota) | xnu-11215 (iOS 18) | iOS 18.7.8 to SpringBoard | SDM845 (**no PAC**) | the proof that arm64e user space runs on a CPU without PAC once rewritten (its Renegade tool) |

Nobody runs iOS 26 on a CPU without PAC. UniXNU's iOS 26 ports all rely on the SM8550's FEAT_PAuth.

## Design decision: rewrite code pages as they are paged in

UniXNU boots iOS 26 from the IPSW's `OS` and `Cryptex1,SystemOS` disk images **unchanged**: Apple's APFS kext mounts
the sealed system volume and checks its seal. Rewriting the binaries on disk breaks the seal (Bergamota copies the
whole system into an HFS+ volume instead).

This port rewrites in memory: XNU already transforms code pages on page-in for encrypted apps (the
`apple_protect_pager` and its `text_crypter` callback, FairPlay's path). A crypter that runs `insn.rewrite()` over each
page of an arm64e mapping gives the same result as rewriting the files, for executables, dylibs and the dyld shared
cache alike, while the sealed volume stays exactly as Apple shipped it. `kernel/pacstrip/pac_rewrite.c` (the C twin of
`insn.py`, checked against its vectors) takes about 15 us per 16 KiB page on the PC; expect roughly twice that on the
A78, close to the cost of reading the page from UFS. It is paid once per page-in, and a pre-filter on the top bits of
each word is still to do.

What the page-in path cannot see is `LC_DATA_IN_CODE` (a data range inside `__text` that looks like a PAC
instruction). Clang puts jump tables in `__TEXT,__const`, so this should be rare; the scan of the real cache measures
it, and the exec/mmap path can pass the ranges to the pager if it is not.

The kexts prelinked into the kernelcache are rewritten statically, at prelink time, with `tools/pacstrip`.

## The plan

### Phase 1: the PAC gap, on the host (no phone needed)

- [x] `tools/pacstrip`: rewrite one Mach-O (our implementation; Renegade states no license, so none of its code is used)
- [ ] the iOS 26 dyld shared cache: rewrite every executable mapping of the main cache and the sub-caches, honour each
      image's data-in-code, refresh the CodeDirectory page hashes
- [x] a feature scan (`featscan.py`): every instruction beyond the A78's features (FJCVTZS, FCMA, LRCPC2, FlagM, ...)
      with its count, LLVM's disassembler as the oracle
- [x] run it on the real iOS 26 cache: 12.5M to rewrite, 3,007 to emulate (LDRAA/LDRAB/FJCVTZS only) — see docs/phase1-featscan.md
- [ ] the Apple kexts the iOS 26 kernelcache contributes (AMFI, APFS, sandbox, IOSurface, ...) are arm64e too: same
      rewrite, and the prelinker must emit unsigned pointers for their authenticated fixups

### Phase 2: the kernel, in QEMU with a CPU without PAC

- [x] UniXNU's Linux toolchain and its iPadOS 26.5.2 profile, built as a **plain arm64 kernel** (no ptrauth) — xnu-12377 links to a standalone MH_EXECUTE, cpusubtype ARM64_ALL, ios 26.0; see docs/phase2-build.md
- [ ] kernel patches: accept arm64e Mach-O (cpusubtype and ptrauth ABI version), never touch the PAC key registers,
      emulate the reported traps in the undefined-instruction handler
  - [x] never touch the PAC key registers: the kernel builds plain arm64 (`HAS_APPLE_PAC` off), so it emits no
        key-register writes or sign/auth code at all
  - [x] emulate the reported traps (LDRAA/LDRAB/FJCVTZS) in `handle_uncategorized`: `pac_emulate.c` (host-tested) +
        `pac_trap.c` glue, linked into the kernel (`_pac_emulate_user_trap` in the symbol table); see docs/phase2-emulation.md
  - [ ] accept arm64e Mach-O in the loader (`bsd/kern/mach_loader.c` cpusubtype/ptrauth-ABI grading)
- [x] `kernel/pacstrip/pac_rewrite.c`: the rewrite in C, checked against 1 018 759 vectors from `insn.py`
- [ ] the PAC-stripping pager: a `text_crypter` for every executable mapping of an arm64e Mach-O and of the shared
      cache, calling `pac_rewrite_buffer()`
- [ ] boot it under `qemu-system-aarch64 -cpu cortex-a76` (ARMv8.2, no PAC: the phone's limits on the host) to
      launchd, then SpringBoard

### Phase 3: lisa

- [ ] boot path: Mu-Silicium `lisaPkg` with UniXNU's UEFI modules (UniXNUDxe, DarwinBoot), started from RAM with
      `fastboot boot` first, nothing written to the phone
- [ ] SoC layer: the SM7325 support (UniXNU's SM7325 port, HakiOS's redwood drivers)
- [ ] lisa's own parts: the CSOT VTDR6130 panel in DSI command mode with DSC, Goodix Berlin touch over SPI, the
      TFA9873 amplifiers (our mainline port documents all of them)
- [ ] the iOS 26 root on the phone's storage

## Inputs that are not in this repository

- an iOS or iPadOS 26 IPSW, obtained by you from Apple;
- the phone's firmware, pulled from your own phone;
- the toolchains, built from their sources.
