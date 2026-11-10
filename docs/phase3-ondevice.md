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

## Boot progress (2026-11-09, session 3): interrupts work; kernel idles at IOKit

The "interrupts wall" is broken. Root cause: `HAS_GIC_V3` was defined only for VMAPPLE, so on our
BCM2837 config `pe_init_fiq()` compiled to the empty stub and the kernel never set up any interrupt
controller. Fixes (kernel/patches/, via apply.sh):
- `bcm2837-gic-v3` — define `HAS_GIC_V3` + the GICv3 register layout for BCM2837 (values identical to
  VMAPPLE.h and to HakiOS's proven lisa GIC: RD_base+SGI_base at +0x10000, 0x20000 per redistributor).
  Now the stock `pe_init_fiq()` maps `/arm-io/gic`, wakes the redistributor, and opens the CPU interface.
- `pe_fiq-vtimer-group1` + `kernel/soc/gic_vtimer.c` — route the EL1 virtual timer (PPI 27) to
  Non-secure Group 1 (IRQ) instead of stock Group 0 (FIQ, which the Qualcomm hypervisor takes to EL2),
  and register a handler via `ml_install_interrupt_handler` (sleh_irq -> cdp->interrupt_handler, since
  BCM2837 has no `USE_APPLEARMSMP`). The handler acks ICC_IAR1_EL1, runs rtclock_intr for intid 27, EOIs.

Confirmed on hardware: `[GIC] vtimer G1 armed IGROUPR0=89ffffff IGRPMODR0=00000000` and
`[GIC] first IRQ at EL1, intid=0x01b` (27 = vtimer). The timer fires, services rtclock and re-arms; ticks
15/16 print **after** `IOServicePH did 0xf->0xf`, so the kernel runs past IOServicePH. It then goes deep
idle (waiting for the root device) and the vtimer stops (nothing to schedule). A Qualcomm hv watchdog then
resets the phone at ~25s in no-bootcpu runs; bootcpu=7 runs stay alive (the loader pets from core 0).
Open: keep the vtimer ticking through idle (delayed thread_calls never re-fire) — under analysis.

## Phase 3 prelink: infrastructure works, blocked on native __DATA layout

`unixnu`'s prelink (`kits/XnuPrelink/emit_prelink.py`, a git submodule) links the Apple kexts from the real
iOS 26 kernelcache into our native kernel. The build-tree copy of the kit was empty (submodule not synced);
symlinking `tree/kits/XnuPrelink -> research/UniXNU/kits/XnuPrelink` makes it runnable. Inputs are present and
correct: `out/ipsw/23F84/kernelcache.decompressed` (sha256 0f7d50ac…, matches the profile pin) and our
`mach.development.bcm2837.unstripped.noctf`. The spec (profiles/os/ipados-26.5.2-23F84/profile.toml
[kernel.prelink]) links 31 roots incl. `com.apple.filesystems.apfs`, AMFI, sandbox, IOStorageFamily, corecrypto.

The prelink **matches cleanly**: it pairs our 28794 functions against Apple's 21113, resolves anchors/data/
sysctls, and reports `conflicts: none`. It then fails only at segment layout:
`ValueError: Native __DATA overlaps a retained segment` (fileset_layout.py:206). Cause: `place_kernel` reserves
our `__DATA` at the kernelcache's `__DATA` root VA (fileset_layout.py:110), but our DEVELOPMENT kernel (36%
more functions than Apple's release kernel) has a larger `__DATA` that overflows into the next retained kext
segment. Resolving this needs either a smaller kernel (RELEASE config / trimmed DEVELOPMENT) or extending the
layout to relocate retained segments / grow the `__DATA` mapping. This is the next Phase 3 sub-task.

## Phase 3 prelink: now runs END TO END; blocker is symbol-matching quality (2026-11-09, session 3 cont.)

With a chain of mechanical fixes the UniXNU prelink (`kits/XnuPrelink/emit_prelink.py`) now runs all the
way through emit for our plain-arm64 (no-PAC) kernel. The blockers resolved, in order:

1. **Kit not staged** — the XnuPrelink submodule was not synced into the build tree. Symlink
   `tree/kits/XnuPrelink -> research/UniXNU/kits/XnuPrelink`.
2. **`Native __DATA overlaps a retained segment`** — our `__DATA` was 3.84 MB (overflow 1.75 MB) because of
   our OWN `loader_console` debug buffers (`loader_log` 1.5 MB + `loader_log_scratch` 1 MB). Shrunk
   `LOADER_LOG_SIZE` 1536 KiB -> 256 KiB (boot logs are ~18 KiB). `__DATA` -> ~1.79 MB, fits.
3. **`Native image has no split-segment relocation information`** — plain arm64 got no split-seg. Added
   `LDFLAGS_KERNEL_ONLY_SUBARCH_arm64 = -Wl,-add_split_seg_info` in `makedefs/MakeInc.def` (only arm64e/x86_64
   had it). Emits `LC_SEGMENT_SPLIT_INFO`.
4. **`Split-segment pointer disagrees with fixup`** — our position-dependent link emitted absolute pointers,
   no rebase info. Added `-Wl,-fixup_chains -Wl,-kernel` to the arm64 subarch ldflags and built with
   `XNU_KERNEL_CHAINED_FIXUPS=1` (force a relink; delete `mach.*.unstripped*` first). Emits
   `LC_DYLD_CHAINED_FIXUPS`.
5. **`Unsupported chained pointer format 6`** — plain arm64 `-fixup_chains` emits
   `DYLD_CHAINED_PTR_64_OFFSET` (format 6); the kit only parsed `ARM64E_KERNEL`(7)/`64_KERNEL_CACHE`(8). Added
   format-6 parsing to `kits/XnuPrelink/kc_image.py` `fixups()` (target[0:36], high8[36:44], next[51:63]
   stride 4, bind bit 63).

The prelink then pairs our 28794 functions against Apple's 21113, relocates every chained fixup, and reaches
the final `write_candidate`, failing only on **`Incomplete pairing: 53075 unresolved references`**. The matcher
names only **544/3094 kext-facing targets** by heuristics (function shapes, string/PAC anchors). This is the
real remaining work and it is NOT mechanical:
- Best lever: pass **`--fileset-reference`** — a symbol-bearing xnu-12377 kernel (a KDK/development kernelcache
  with a symbol table) so the matcher maps names directly instead of guessing. We do not have one yet.
- A **RELEASE** build (Apple's config) would also match far better than our DEVELOPMENT kernel (36% more
  functions, different inlining). The build script hardcodes `DEVELOPMENT_ARM64_BCM2837` in ~dozens of places.
- `--allow-missing` would stub the 53075 refs, but apfs/AMFI/sandbox each call hundreds of kernel functions,
  so a stubbed cache would trap early — not a shortcut to a booting rootfs.

So: the prelink MECHANICS are solved (it runs end to end and emits); the gating problem is now symbol-matching
quality, which needs a reference kernel (or a RELEASE build + the apple-oss reference sources the board's
`[[source]]` list points at) plus matching tuning. That is the next Phase 3 sub-project.

### Experimental build changes (in the work tree; formalize as patches when pursued)
- `makedefs/MakeInc.def`: `LDFLAGS_KERNEL_ONLY_SUBARCH_arm64 = -Wl,-add_split_seg_info -Wl,-fixup_chains -Wl,-kernel`.
- `kits/XnuPrelink/kc_image.py`: parse `DYLD_CHAINED_PTR_64_OFFSET` (format 6). This one is a clean, keep-it fix.
- `kernel/loader-console/loader_console.c`: `LOADER_LOG_SIZE` 1536->256 KiB (committed to darwin-lisa).
- These make the native kernel a chained-fixups/kernelcache-targeted image, so it no longer boots standalone;
  the bring-up kernel (kernel26.nopac) must be rebuilt WITHOUT these flags for on-device bring-up.

## Timer-at-idle: confirmed a bring-up artifact, deprioritized
Two Opus deep-analyses (resumable agent) concluded the "vtimer stops at idle" is NOT a kernel/GIC bug but the
single-wake-source starvation of a driverless single-core bring-up kernel: the vtimer is the only thing that
can wake WFI, and this Qualcomm hv does not re-inject a pure guest-vtimer deadline as a WFI wake. HakiOS (full
iOS) never hits it because device IRQs + scheduler SGIs keep the cores cycling. So the prelinked kernel (real
kexts + userland + IPIs) will not hit this. Tried and rejected as fixes: EOImode=1+ICC_DIR (ap1r0 was constant,
not a priority lockout), Group-1-only, `wfi=0`, `jtag=1 wfe_mode=0`, a 50 ms CNTV_TVAL floor. Left as-is; revisit
with the real driver stack. (The tick-12 WFI probe that "hung" was confounded: it WFI'd mid-handler with INTID
27 still Active, which a level IRQ cannot re-forward as a wake — a GIC artifact, not hv evidence.)

**Update 2026-11-10 (full iOS 26 boot, cpufreq + SMP work, `kernel/soc/smp/README.md`):** the idle loop polls by
default now (`kernel/patches/cpu-idle-no-wfi.patch` in `apply.sh`; `wfi=1` brings WFI back). The cpufreq boot that
"hung" on the device turned out to be a display-driver panic (bzero on the Device-mapped panel), invisible as a panic
because `loader_return()` does nothing on the 16 KB kernel. With several cores (SGI 0 wired as the IPI) the WFI
question is open again: not yet measured whether an SGI or the vtimer wakes a core parked in WFI under this hv.

## Prelink emits a real MH_FILESET kernelcache (with --allow-missing)

Running `emit_prelink.py --allow-missing` (over the strict pairing gate) **writes an 85 MB
`kernel.prelinked.macho`, a valid MH_FILESET (0xc) kernel collection** holding our kernel plus ~30
retained Apple kexts as fileset entries. The strict "53075 unresolved references" is the total
cross-reference count; the actual closure is far smaller: **871 unresolved closure references, 5802
stubbed functions** (`runtime qualification: pending`). A symbol-bearing reference kernel
(`--fileset-reference`) should drive both down; what matters for a first boot is that the boot-critical
kexts (apfs, AMFI, sandbox, IOKit families) resolve, not the long tail.

**Boot-path note (future):** our prelink output is MH_FILESET, but the HakiOS `xl` loader loads a
standalone MH_EXECUTE kernel (HakiOS's own kernel is MH_EXECUTE with its drivers compiled in + a
ramdisk; it does NOT use a fileset kernelcache). So booting our kernelcache needs `xl`/xnuloader to
learn the fileset/kernel-collection boot path (map the collection, set up prelink info, enter the
kernel's entry within it) — a separate task, after matching is good enough to produce a bootable cache.

Reproducibility: `phase3/prelink/apply-build-flags.sh <work>/makedefs/MakeInc.def` adds the arm64 link
flags; `phase3/prelink/kc_image-format6.patch` teaches the kit format 6; rebuild with
`XNU_KERNEL_CHAINED_FIXUPS=1` (delete `mach.*.unstripped*` to force a relink), then run the prelink.

## Reference kernel (KDK) drops the matching gap ~4x (2026-11-09)

Got a symbol-bearing xnu-12377 reference kernel: the macOS 26.5.2 (build 25F84, xnu-12377.121.10 — exact
match to iPadOS 26.5.2) **Kernel Debug Kit**, mirrored on GitHub by **dortania/KdkSupportPkg** (no Apple
account). Acquire on Linux:
- `curl -L -O https://github.com/dortania/KdkSupportPkg/releases/download/25F84/Kernel_Debug_Kit_26.5.2_build_25F84.dmg`
- `7z x` the dmg -> `KernelDebugKit.pkg`; `7z x` the pkg -> `KDK.pkg/Payload` (pbzx); decode pbzx (xz chunks)
  and pipe to `cpio -idm '*/Kernels/*'`. Use `System/Library/Kernels/kernel.release.t6000` (arm64e,
  MH_EXECUTE, **6894 nlist symbols**). Kept at `~/Dev/mobile/refs/kdk/kernel.release.t6000` (not in git, 16 MB).

Passing `--fileset-reference .../kernel.release.t6000` to emit_prelink:

| metric | no reference | with KDK reference |
| --- | --- | --- |
| kext-facing targets named | 544/3094 | **2499/3094** |
| unresolved closure references | 871 | **95** |
| stubbed functions | 5802 | **1378** |

The matcher now names Apple's stripped iOS functions via the reference's symbol table (602 symbol anchors,
3695 order anchors, 185 data names) instead of guessing by shape. The 85 MB MH_FILESET still writes.

BUT the remaining missing/stubbed are in **boot-critical kexts** — apfs (22 missing + 117 stubbed), AMFI
(3+65), AppleARMPlatform (10+80), IOHIDFamily (11+...). apfs/AMFI call kernel functions our build didn't
match, so the cache would still trap during root mount / code-signing. The gap is our native kernel being
**DEVELOPMENT** (28794 fns, different inlining) while both the iOS cache kernel and the KDK reference are
**RELEASE**: shape matching native<->original suffers. Next lever: a **RELEASE** native build, which should
drive the critical stubs toward zero. Then pacstrip the arm64e kexts (no-PAC A78) and boot (the `xl` loader
is already largely fileset-ready: it maps LC_SEGMENT_64 + jumps to the top-level LC_UNIXTHREAD, both present;
no filetype check; 85 MB fits kernel_size/MAX_MEM_SIZE; XNU handles the collection via __PRELINK_INFO).

## Caminho B: reuse HakiOS's proven iOS-16 stack (pivot, 2026-11-09)

The prelink path (matching + building a kernelcache from Apple's binary kexts) reached an 85 MB MH_FILESET
with the KDK reference (2499/3094 matched, 95 unresolved + 1378 stubbed), but the stubs are in boot-critical
kexts (apfs/AMFI/platform). Rather than push RELEASE + more matching, we pivoted to HakiOS's approach, which
is proven on THIS phone for iOS 16: a standalone MH_EXECUTE kernel with HakiOS's OWN built-in drivers + an
in-RAM APFS root, NO prelink, NO Apple binary kexts. HakiOS reimplemented APFS (bsd/miscfs/apfs/, 15K lines,
read-write) and ships security stand-ins (miniamfi/kern_minimac) so unsigned userland runs; the arm64e
userland runs via our pac-pager (PAC stripped at page-in).

Progress:
- **APFS driver ported + builds** (kernel/apfs/, via kernel/apfs/apfs-wire.py). The 15K-line driver compiled
  essentially unchanged (VFS/vnode/buf KPIs stable 16->26); one diagnostic thread func stubbed. Kernel links
  clean, apfs symbols + `options APFS` present.
- **iOS-26 test root built** (~/Dev/mobile/userland-26/out/root-26.apfs, 4 MB): an mkapfs container with a
  static no-PAC /sbin/launchd (LC_UNIXTHREAD, no dyld), read-verified by the kernel's own apfs_lib. Proves
  mount+exec in isolation (no pac-pager/miniamfi needed for this init). The full system-26.apfs recipe is
  ready (userland-26/scripts/): decrypt the iOS-26 AEA FileSystem/cryptex (keys from Apple's wkms server via
  `ipsw`), lay the cryptex content + dyld cache at /private/preboot/Cryptexes/{OS,App} (iOS 26 ships those as
  symlinks into an empty /private/preboot), keep arm64e binaries unmodified.
- **miniamfi/kern_minimac staged** (kernel/security/) for integration after apfs+platform.

Current blocker (same IOServicePH wall as before the pivot): the kernel boots to
`IOServicePH ... did 0xf->0xf` (IOUserServer/DriverKit power coordination) and does not reach bsd_init's
vfs_mountroot. The loader hands the ramdisk over correctly (`ramdisk=` -> a `RAMDisk` memory-map DT node ->
the stock imageboot/di_root path -> /dev/md0, `rd=md0` auto-added). Next: port HakiOS's core platform/stub
drivers (hakios.platform.c/stubs.c/version.c/...) to unblock IOKit, and/or resolve the single-wake-source
idle stall (a bring-up artifact per the timer analysis; real drivers add wake sources). GPU (Adreno) + touch
(Goodix) are the end-goal minimum but come after the kernel boots to userland.

## MILESTONE (2026-11-09): the FULL iOS 26 root mounts + boots userland on device

Both bring-up workers delivered and the result was confirmed on hardware (lisa, bootcpu=7,
cpus=1, 0 panics):

- **Full root**: `userland-26/out/system-26.apfs`, 3.59 GB, 65688 files / 24183 dirs / 821
  symlinks (one mkapfs System volume). Ships our static init as `/sbin/launchd` + Apple's
  arm64e launchd as `/sbin/launchd.darwin`; dyld cache at
  `/private/preboot/Cryptexes/OS/.../com.apple.dyld`. Build scripts: `userland/scripts/`.
- **Kernel**: `kernel26.nopac` (22,790,856 B) with miniamfi (always on) + Goodix/Adreno
  behind boot-args, on top of the 5 bring-up fixes + APFS/GICv3.

Boot driver `userland/scripts/run-fullroot.sh` (a fresh loader session is required, but
`fastboot reboot` wipes the resident region, so the order is reboot -> `xl ramdisk
system-26.apfs` [the resident upload raises max-download-size past the 63 MB kernel-payload
cap] -> boot with NO `ramdisk=`; the loader prepends `cpus=1 rd=md0` and wires the resident
image as md0). Confirmed log:

```
Added memory device md0/rmd0 (01000000/0E000000) at FFFFFFF00C000000 for 00000000E0838000  <- 3.59 GB
BSD root: md0, major 1, minor 0
apfs: volume 0 "System" of container 0: 65688 files, 24183 directories, 915522 blocks of 4096 used
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
==== iOS 26.5.2 (23F84) userland on lisa: /sbin/launchd alive ====
init26: pid=1 uid=0 argv0=/sbin/launchd
init26: contents of /:   ->  usr  System  private  Applications ...   (the real iOS 26 root)
init26: no /bin/sh in this image, nothing to hand over to
init26: first-boot checks done, halting.
```

So our APFS driver mounts the real, full iOS 26.5.2 filesystem and runs userland to a clean
halt. NEXT: exercise Apple's real arm64e launchd (`/sbin/launchd.darwin`) through miniamfi +
the pac-pager (keep `/bin/sh` or point init at launchd.darwin); then the touch/GPU boot-args
and the deferred Metal stack for the UI.

## dyld shared cache (2026-11-09): WALL #1 solved, WALL #2 = kernel shared-region

Apple's real launchd/dyld runs under the pac-pager; the only thing between us and Apple
userland is loading the dyld shared cache. Two walls, found with on-device evidence:

- **WALL #1 (SOLVED, userland):** code-signing page-size mismatch. The cache is signed with
  pageSize shift 14 (16 KB page hashes — real iOS runs 16 KB kernel pages); our lisa kernel
  runs 4 KB pages, so `cs_validate_codedirectory` rejects shift 14 (EBADEXEC,
  "disallowing unsupported code signature page shift: 14"). Fix: `userland/scripts/resign-cache-4k.py`
  re-signs the 35 cache files to 4 KB-page SHA256 hashes. After it, registration + preflight pass.
  (Fold into build-system-26.sh ONLY on the 4 KB path — do not re-sign if the kernel moves to 16 KB.)

- **WALL #2 (open, KERNEL):** dyld validates the cache but can't MAP it. The kernel reserves
  0x180000000-0x2ffffffff for the arm64e shared region and denies user MAP_FIXED there (EACCES);
  the cache's map0 base is 0x180000000. Private mapping is therefore impossible; the system-wide
  `shared_region_map_and_slide_2_np` owns that range but doesn't populate it
  (`vm_shared_region_start_address()` -> KERN_FAILURE). Also the pac-pager strips PAC only on the
  exec'd main binary, not on the shared-region-mapped cache. Work: osfmk/vm/vm_shared_region.c +
  bsd/vm/vm_unix.c `_shared_region_map_and_slide`, and extend the pac-pager to the cache.

**Decision (page size):** (A) wire the arm64e shared-region map + cache PAC-strip on the 4 KB kernel
(keep the re-sign), or (B) rebuild the kernel at 16 KB pages (PAGE_SHIFT=14) — matches iOS, dissolves
WALL #1, likely fixes the shared region by design; larger change, re-validate APFS/pac-pager, still
needs the cache PAC-strip.
