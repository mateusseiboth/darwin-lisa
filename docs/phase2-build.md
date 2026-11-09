# Phase 2: building xnu-12377 (iPadOS 26.5.2) as a plain arm64 kernel

Progress 2026-11-09. The UniXNU build for the 26.5.2 profile is **set up and compiles up to a single,
well-defined blocker** (the ptrauth headers). This is exactly the darwin-lisa "no ptrauth" frontier.

## What works (the build pipeline, step by step)

A minimal compile-only device and a stub SoC feed are checked in under `phase2/`:

- `phase2/lisa-26-device/` — board.toml (soc sm7325, os `ipados-26.5.2-23F84`, arch via the arm64 profile,
  defconfig `lisa26_defconfig`) + feeds.toml pointing at the local SoC stub.
- `phase2/UniXNU-sm7325-common/` — a stub SoC feed (`soc.toml` + empty configs/patches), enough for the
  UniXNU build to have a `kind="soc"` source for SM7325 (the real SoC layer is Phase 3).

Put both where UniXNU expects them and build **inside the toolchain image**:

```sh
# the device lives at ~/Dev/mobile/lisa-26-device ; the stub feed at ~/Dev/mobile/UniXNU-sm7325-common
# feeds.toml references the feed by a LOCAL git path + commit, so: git init + commit the stub feed first.

cd ~/Dev/mobile/research/UniXNU
python3 unixnu defconfig lisa26_defconfig -D ~/Dev/mobile/lisa-26-device   # writes out/.../.config
python3 unixnu fetch         -D ~/Dev/mobile/lisa-26-device                 # clones xnu-12377, libpthread, corecrypto (+UEFI; slow)

# the kernel compile needs the toolchain at /opt/unixnu-linux, which lives only in the docker image;
# mount the tree at its REAL path so the absolute paths in out/ resolve, and trust it for git:
docker run --rm \
  -v /home/mateusseiboth/Dev/mobile:/home/mateusseiboth/Dev/mobile \
  -w /home/mateusseiboth/Dev/mobile/research/UniXNU -e HOME=/home/mateusseiboth \
  darwin-lisa/unixnu-toolchain:latest \
  sh -c "git config --global --add safe.directory '*'; \
         python3 unixnu build kernel -D /home/mateusseiboth/Dev/mobile/lisa-26-device"
```

- The fetched XNU is commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` = the 26.5.2 profile's pin. ✓
- The 26.5.2 OS patch series carries one patch, `zzz-boot-progress.patch`, which **does not apply**
  (bsd_init.c:778) on top of the arm64 series. It gates `CONFIG_XNU_BOOT_PROGRESS`, which this defconfig
  leaves off, so it was dropped from `profiles/os/ipados-26.5.2-23F84/patches/xnu/series` to proceed.
- With that removed, all patches apply and the build starts **compiling**.

## The blocker (where Phase 2 resumes)

The compile stops building `libdispatch`/firehose (`scripts/build-firehose.sh`, a kernel dependency):

```
sys/ucred.h:164: error: '__ptrauth' qualifier must take between 1 and 3 arguments
  note: os/base.h:355 expanded from OS_PTRAUTH_SIGNED_PTR_AUTH_NULL ...
        __ptrauth(ptrauth_key_process_independent_data, 1, ptrauth_string_discriminator(type), "authenticates-null-values")
```

- `os/base.h` guards the 4-arg form with `#if __has_feature(ptrauth_calls)`; the else branch is a no-op.
  So during this compile `__has_feature(ptrauth_calls)` is **true**.
- The toolchain's compiler is **stock Ubuntu clang 18.1.3** (plus UniXNU's `__builtin_xnu_type_*` patch).
  Stock clang 18 supports `__ptrauth` with **1–3 arguments only**; the 4-argument form (the
  `"authenticates-null-values"` string, new in Darwin 26) is an Apple-clang extension it rejects.
- Oddly, `__has_feature(ptrauth_calls)` is **false** for every isolated `clang -target arm64[e]-apple-ios{17,26}
  [-mkernel]` test in the image; it only turns true inside firehose's real compile (full SDK + `-mkernel`
  include chain). Patching the fakeroot `os/base.h` by hand does not stick — the build restores it root-owned.

## Next steps (Tuesday)

This IS the darwin-lisa "plain arm64, no ptrauth" work:

1. **Neutralise ptrauth in the build pipeline** (preferred, matches the roadmap): make the plain-arm64 build
   compile with `__has_feature(ptrauth_calls)` false / the `OS_PTRAUTH_*` macros no-op — e.g. a header fixup
   step (like build-firehose.sh's unifdef / COPY_HEADERS) that rewrites the 4-arg `__ptrauth` in the fakeroot
   SDK headers, or a compiler flag/define that forces the no-ptrauth branch. Then firehose and the kernel
   compile, and the kernel never emits PAC.
2. OR rebuild the UniXNU toolchain from a clang that accepts 4-arg `__ptrauth` (if UniXNU has a newer one).
3. Then: the darwin-lisa kernel patches (accept arm64e Mach-O, never touch PAC key regs, emulate
   **LDRAA/LDRAB/FJCVTZS** — the only three from Phase 1) + wire `kernel/pacstrip/pac_rewrite.c` as the
   pager's `text_crypter`, and boot under `qemu-system-aarch64 -cpu cortex-a76`.

The compile-only device needs no device firmware/kernelcache (that is for prelink/boot, Phase 3). The real
SM7325 SoC feed + drivers (from the HakiOS/redwood port) are also Phase 3.
