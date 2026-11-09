# Phase 3: booting iOS 26 on lisa — first on-device boot

Updated 2026-11-09. The darwin-lisa kernel **boots on the real phone** (Xiaomi 11 Lite 5G NE, SM7325,
Cortex-A78, no PAC) over the existing HakiOS `xl` path — no UEFI, no pmOS, no physical access.

## First on-device boot (2026-11-09)

Swapping our plain-arm64 `kernel.macho.nopac` in for `hakios-kernel` and running it through the xnuloader
(`fastboot stage` + `oem run`, cmdline `hv_wdt_pet=1 loader_return_after=15 debug=0x8 -v`), the loader found
our return slot, jumped to the kernel, and the kernel ran and handed its log back:

```
return slot patched at f7b74118, checkpoint 0
jumping to f751e488
kernel returned: log at f7c24524, 274 bytes
== KERNEL LOG ==
--- kernel message buffer ---
[   12.455000]:  vm_cache_geometry_colors: 32
[   12.455016]: do_cacheid() - 65536 bytes Unknown cache (I:32768 D:32768 (separate)), 16-way assoc, 64 bytes/line
[   12.455030]: [processor_init] processor 0 belongs to processor set 0 of type 0
```

So xnu-12377 runs on the A78 far enough to do **VM init, cache detection** (the A78's real 32 KiB+32 KiB
16-way L1) and **processor_init**, then stops in early `kernel_bootstrap`. The full log is in
`logs/first-boot-26.log`. This proves the whole stack: the plain-arm64 no-PAC kernel loads via the generic
XNU boot_args path, the loader's ADT is parsed far enough for early init, and the return-to-loader channel
(`kernel/loader-console/`) makes it observable over fastboot.

### How to reproduce (needs the phone in the loader)

```sh
cd ~/Dev/mobile/research/hackios-xnu-kernel
cp <our>/kernel.macho.nopac hakios-kernel         # swap our kernel in (keep a backup!)
fastboot reboot && wait-for xnuloader             # a FRESH loader session (one kernel per session)
xnuloader/mkpayload.py -o run-payload.bin kernel=hakios-kernel \
  --cmdline "loader_return_after=15 hv_wdt_pet=1 debug=0x8 -v"
fastboot stage run-payload.bin && fastboot oem run
# wait for result "returned N 0", then fetch WITHOUT "oem log" first (it re-stages loader-only):
fastboot get_staged out.txt                       # out.txt = loader log + our kernel log
```

Key points learned: run the kernel on the loader's generic ADT; `hv_wdt_pet=1` feeds the Qualcomm watchdog
so it does not hard-reset before the return; one kernel per loader session (reboot between runs); do not call
`oem log` before `get_staged` on a return (it re-stages the loader log alone).

## What remains to boot further

The kernel stops in early `kernel_bootstrap` (right after processor_init), almost certainly at interrupt
controller / SMP / platform bring-up on hardware it only partly understands via the loader's ADT. Getting
further is the SM7325 SoC layer below. This is now a normal debug loop: each change, boot, read the log.

## Older plan notes (kernel side, still accurate)

The darwin-lisa **kernel is done and staged**; this is what remains to see it run on the
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

## Boot progress (2026-11-09, session 2): into corecrypto FIPS POST

With the loader log channel + live console capture, the bring-up became a normal debug loop. Fixes applied
(all in kernel/patches/, via apply.sh):
- `prng-seed-bringup` — fill the short bootloader random seed instead of panicking.
- `pe_init-skip-console` — no-op the VRAM/framebuffer console (its mapping faults).
- `machine-routines-virtual-timer` — don't touch CNTP_* (the physical timer is trapped by the phone's
  Qualcomm hypervisor); the guest owns only the virtual timer.
- `debug-loader-return` / `serial-console-tee` — log panic reason + live console into the loader channel.

The kernel now boots on the A78 through: VM bootstrap (24G zones, 6G compressor), scheduler (clutch), OSLog/
Firehose, the kext bootstrap, `corecrypto_kext_start`, and the FIPS power-on self-tests (passes
`fipspost_post_hmac`). Full log in logs/boot-corecrypto-26.log.

### Current blocker: source-corecrypto FIPS integrity self-test

`fipspost_post_integrity` → `fipspost_get_hmac` hashes the kernel's `__text`. For a non-cache kernel it uses
`pmach_header + sect->offset` (a flat-file layout). Our kernel is source-linked and runs under XNU's
segmented pmap, so that address does not point at `__text` by corecrypto time — the hash runs on the wrong
region and never completes. `prng_random.c:605` forbids `fips_mode != 0`, so FIPS cannot be skipped in source
mode. Two ways forward:
1. Patch `fipspost_get_hmac` to hash `__text` by its vmaddr+slide (what XNU's pmap maps), matching the seal's
   content; keep re-sealing the **stripped** kernel (seal AFTER pacstrip — the 176 XPAC→NOP edits change the
   hashed bytes).
2. Switch to prelinked mode (corecrypto becomes a real kernelcache kext, the cache path works) — needs the
   full prelink against the IPSW kernelcache (the deferred Phase 3 prelink work).

Note: the kernel must be **pacstripped then re-sealed** (`scripts/seal-source-crypto-kernel.py` on the
`.nopac`), since pacstrip edits `__text` after the build's seal.

## Correction: the FIPS "hang" was slow boot, not a blocker

The corecrypto integrity self-test does NOT hang — it passes in ~257 ms. The earlier capture windows were
too short for the slow boot (software crypto, low boot clock). With a long window the FIPS POST runs and
passes test after test: integrity, indicator, aes_ecb/cbc, rsa_sig, ecdsa, ecdh, aes_ccm, aes_cmac, hkdf
(11 passes captured; see logs/boot-fips-passing-26.log). The `corecrypto-integrity-vmaddr` patch idea was
unnecessary and was dropped; the stock immutable corecrypto works (`__text` is flat-mapped, base+offset == vmaddr).

## The real wall: interrupts (the SM7325 GIC)

With `bootcpu=7` the loader runs on core 0 (answers USB, reports `result: running`) while the kernel runs on
core 7. The kernel never takes the `loader_return_after` timed return, because that timer needs **timer
interrupts**, and there is no working interrupt controller: XNU logged `failed to find the interrupt-controller`.
The live log stops right after `fipspost_post_hkdf`, at the DRBG self-test, which needs **entropy** — and
entropy is accumulated from **interrupt timing**. No GIC -> no interrupts -> no entropy and no timers, so the
DRBG and everything scheduler/timer-driven stalls.

So the next real work is the SM7325 interrupt controller (GICv3) + the virtual-timer interrupt (PPI 27): make
XNU find/drive the GIC the loader's ADT describes. That unblocks entropy, the DRBG, the timed return, and the
scheduler — i.e. the rest of the boot toward launchd. This is the core of the SM7325 SoC layer.
