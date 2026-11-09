# Phase 2: xnu-12377 (iPadOS 26.5.2) built as a plain arm64 kernel — DONE

Updated 2026-11-09. The UniXNU build of the 26.5.2 profile now produces a **complete, standalone,
plain-arm64 (no-ptrauth) XNU kernel from source**. This is the darwin-lisa Phase 2 goal.

## Result

```
build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837   (17.5 MB)
  magic       0xfeedfacf   MH_MAGIC_64
  cputype     ARM64
  cpusubtype  ARM64_ALL (0x0)       <-- plain arm64, NOT ARM64E; no PAC ABI
  filetype    MH_EXECUTE            <-- standalone kernel
  LC_BUILD_VERSION  platform=iOS  minos=26.0.0  sdk=26.0.0
```

The whole xnu-12377 source tree compiles with the stock-clang-18 toolchain, the source corecrypto
and libpthread archives link in, the ArmDTPlatform/ArmCpu components link in, and the final kernel
links clean. `kernel.log` ends with `Source kernel POST integrity field populated`.

## How to reproduce

The compile-only device is `~/Dev/mobile/lisa-26-device` (board.toml + `lisa26_defconfig`); the stub
SoC feed is `~/Dev/mobile/UniXNU-sm7325-common` (git-committed, referenced by `feeds.toml`).

```sh
cd ~/Dev/mobile/research/UniXNU
python3 unixnu defconfig lisa26_defconfig -D ~/Dev/mobile/lisa-26-device
python3 unixnu fetch                       -D ~/Dev/mobile/lisa-26-device   # xnu-12377, libpthread, corecrypto-2023, ...

# the kernel compile needs the toolchain at /opt/unixnu-linux, which lives only in the docker image;
# mount the tree at its REAL path so absolute out/ paths resolve, and trust it for git:
docker run --rm \
  -v /home/mateusseiboth/Dev/mobile:/home/mateusseiboth/Dev/mobile \
  -w /home/mateusseiboth/Dev/mobile/research/UniXNU -e HOME=/home/mateusseiboth \
  darwin-lisa/unixnu-toolchain:latest \
  sh -c "git config --global --add safe.directory '*'; \
         python3 unixnu build kernel -D /home/mateusseiboth/Dev/mobile/lisa-26-device"
```

`build kernel` runs a **bootstrap pass** first (XNU_BOOTSTRAP=1) that compiles everything and links
*without* the archives, only to leave the compile records (`.o.json`); its link is allowed to fail and
kernel.py swallows it as long as `osfmk/DEVELOPMENT/startup.o.json` appears. Then `build_archives`
builds the recipes from those records, and the **real pass** links the kernel with the archives.

> After the kernel links, `build kernel` continues into **prelink** (`kits/XnuPrelink/emit_prelink.py`,
> fetches the IPSW). That is Phase 3 (a signed kernelcache for on-device boot) and is not present in this
> compile-only tree, so it stops there. The kernel Mach-O above is already complete and is the Phase 2
> deliverable.

## The four changes that got it to link (vs. the stock 26.5.2 arm64e profile)

UniXNU's 26.5.2 profile targets an **arm64e** kernelcache (PAC on, prelinked crypto). We retarget it to a
plain-arm64 source build. All of these live in the device repo or are local tree edits (not yet upstream):

### In the UniXNU tree (`research/UniXNU`, local edits)
1. `profiles/arch/arm64e/profile.toml` → `[arch] name = "arm64"`. Keeps the 26-compatible arm64e **patch
   series** (the plain `arm64` series fails at `start.s:697`, architectural-timer-entry.patch) but makes the
   compile **target** plain `arm64` (no ptrauth).
2. `profiles/os/ipados-26.5.2-23F84/patches/xnu/series` → emptied (dropped `zzz-boot-progress.patch`, which
   fails at bsd_init.c:778 and gates CONFIG_XNU_BOOT_PROGRESS, off here).
3. `patches/xnu-12377/fileset-unmapped-call.patch` → `__builtin_ptrauth_strip(__builtin_return_address(0),0)`
   becomes plain `__builtin_return_address(0)` (debug.c:2590; the no-ptrauth target has no ptrauth builtins).
4. `scripts/build-arm-driver.py` (+`-Wno-error=overriding-option`) and `scripts/unixnu/kernel.py`
   (`UNIXNU_SDK_VERSION`/`DARWIN_TRIPLE` 17.0 → **26.0**, i.e. `arm64-apple-ios26.0`, still no-ptrauth) so the
   ArmDTPlatform/ArmCpu component objects pass the deployment-version audit (object must be ios 26.0, 0x1A0000).

### In the device repo (`~/Dev/mobile/lisa-26-device`, the clean board-local part)
5. `configs/lisa26_defconfig` → `CONFIG_XNU_CRYPTO_BOOTSTRAP="source"` (not `"prelinked"`), and `board.toml`
   drops the `os:configs/ipados.config` fragment (its only line forces prelinked). **Why:** in `prelinked`
   mode `prng_random.o` is compiled with `-UXNU_SOURCE_CORECRYPTO14`, so it does not define
   `source_corecrypto14_original` / `source_corecrypto14_start_once`, yet OSKext (built with the global
   `-DXNU_SOURCE_CORECRYPTO14=1`) references them → undefined symbols at link. `source` mode keeps the macro
   on `prng_random.o`, defines the glue, links the corecrypto source archive, and seals it.
6. `board.toml` `[kernel] archives = [...]` — the arm64e arch profile ships `archives = []` (it links a
   prelinked kernelcache); we supply the source-archive plan (mirrors `profiles/arch/arm64`):
   `build-corecrypto-14.py`, `build-corecrypto-14.py --namespace`, `component:ArmDTPlatform`,
   `component:ArmCpu`, `build-libpthread-kernel.py`. board.toml is merged last, so its list replaces the
   profile's empty one.

## Next (Phase 3 — the darwin-lisa no-PAC runtime)

The kernel builds; now make it *run* real arm64e userland on the no-PAC A78:

1. darwin-lisa kernel patches: accept arm64e Mach-O (cpusubtype + ptrauth ABI), never touch the PAC key
   registers, and emulate the only three instructions Phase 1 found — **LDRAA / LDRAB / FJCVTZS** — in the
   undefined-instruction handler.
2. Wire `kernel/pacstrip/pac_rewrite.c` as the PAC-stripping pager's `text_crypter` (docs/pac-pager.md);
   Phase 1 measured 12.5 M rewrite + 3,007 emulate on the real 26.5.2 cache, so the design holds.
3. Boot under `qemu-system-aarch64 -cpu cortex-a76` to launchd, then SpringBoard.
4. Then the real SM7325 SoC feed + drivers (from the HakiOS/redwood port) replace the stub feed.
