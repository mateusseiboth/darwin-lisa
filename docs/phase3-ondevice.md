# Phase 3: booting iOS 26 on lisa — plan and honest status

Updated 2026-11-09. The darwin-lisa **kernel is done and staged**; this is what remains to see it run on the
phone (Xiaomi 11 Lite 5G NE, SM7325, Cortex-A78, no PAC).

## What is ready

`unixnu prelink --skip-prelink` stages our native kernel (with the EL0 emulator, the PAC-stripping pager and
the arm64e grade all linked in) to a boot-ready Mach-O:

```
out/lisa26/ipados-26.5.2-23F84/lisa26/kernel/kernel.macho       native, 21 MB
out/lisa26/ipados-26.5.2-23F84/lisa26/kernel/kernel.macho.nopac pacstrip'd, 0 PAC instructions
```

The `.nopac` copy is what a Cortex-A78 (no FEAT_PAuth) must run: `tools/pacstrip strip` reduced its only 176
XPAC words to zero. This is the kernel the loader hands control to.

## The remaining chain (each piece is real work, not a flag)

1. **A boot path for lisa.** iOS 26 on UniXNU boots through UEFI: a Mu-Silicium firmware (`lisaPkg`) built
   with UniXNU's `DarwinBoot` + `UniXNUDxe`, started from RAM with `fastboot boot` (nothing written to the
   phone, so pmOS stays intact). Building it means fetching **edk2** (large), building its BaseTools, then the
   firmware — and generating the lisa device tree (not the QEMU `make-dt.py` one). The phone already has the
   U-Boot dual-boot menu, so a `fastboot boot <image>` from the loader is the entry point.
2. **The SM7325 SoC layer.** Our kernel is the generic 4K (BCM2837) config: it has **no lisa drivers**. Even
   booting through UEFI, it has no display, UFS or interrupt controller for this phone, so it cannot show
   anything or mount a root yet. This is the big Phase 3 port: the SM7325 support (UniXNU's SM7325 feed) plus
   the lisa parts from the mainline/HakiOS work — CSOT VTDR6130 panel (DSI command mode, DSC), Goodix Berlin
   touch over SPI, TFA9873 amps, the Adreno 642L GPU. The HakiOS (iOS 16) kernel already drives all of these;
   they must be rebased onto xnu-12377.
3. **A prelinked kernelcache (optional first, required later).** `--skip-prelink` (used above) boots the bare
   kernel without the Apple kexts. A real boot to launchd needs the kexts (AMFI, APFS, sandbox, IOSurface…)
   from the IPSW kernelcache, prelinked in via `kits/XnuPrelink` and PAC-stripped at prelink time (Phase 1's
   open "Apple kexts" item). The IPSW is already extracted under `ipsw/26.5.2-extracted/`.
4. **The iOS 26 userland** on the phone's storage (the OS and Cryptex1,SystemOS volumes), served sealed and
   rewritten by the pager, exactly as the pager design intends.

## Recommended order

The boot path (1) and the SoC layer (2) are the gating Phase 3 sub-projects; neither is a quick step, and (2)
is the larger — it is a driver port, the same scope as the original HakiOS bring-up but on the 26 kernel. A
realistic first on-device milestone is **UEFI firmware + native kernel booting far enough to emit early kernel
log over a working lisa serial/RAM-console**, before any display. Everything above the kernel (emulator,
pager, grade) is already in place and waiting for that bring-up.
