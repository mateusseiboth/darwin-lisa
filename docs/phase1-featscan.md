# Phase 1 result: the real iOS 26.5.2 cache, scanned

Done 2026-11-11 on the arm64e dyld shared cache of **iPadOS 26.5.2 (23F84)**, extracted from the IPSW.
This is the roadmap's open Phase 1 item ("run it on the real iOS 26 cache, so we know what the kernel
must emulate"), and the answer is small and encouraging.

## Numbers

Across the whole arm64e cache (33 sub-caches, `tools/pacstrip/pacstrip.py scan`):

- **12,536,018** PAC instructions to **rewrite** on page-in (RETAB/BLRAA/PAC*/AUT*/LDRAA-in-range, …) —
  the pager handles these with `pac_rewrite_buffer()`.
- **3,007** instructions to **emulate** (cannot be rewritten in place), and they are only **three kinds**:

  | Instruction | Count | What the kernel does |
  |---|---:|---|
  | `LDRAA` | 1,514 | authenticated load with an offset outside the rewritable window → plain `LDR` at the (unauthenticated) address + offset |
  | `LDRAB` | 1,021 | same, B-key |
  | `FJCVTZS` | 472 | FEAT_JSCVT (JavaScript float→int); the A78 lacks it → emulate the convert + flags |

So the undefined-instruction handler the kernel needs (Phase 2) only has to cover **LDRAA, LDRAB and
FJCVTZS**. No surprises (no FCMA/LRCPC2/FlagM/etc. survive the rewrite), which means the pacstrip design
holds on the real 26 cache.

## How it was extracted (Linux, no macOS)

The 26 IPSW ships the OS as AEA-encrypted DMGs. Tooling installed on the host:

- `ipsw` (blacktop) v3.1.743 at `~/Dev/mobile/tools/ipsw-dl/ipsw` — decrypts AEA in pure Go (fetches
  the key from Apple), mounts the APFS and pulls the cache.
- `apfs-fuse` (`dnf install apfs-fuse`) — ipsw mounts the DMG through it.

```sh
~/Dev/mobile/tools/ipsw-dl/ipsw extract --dyld --dyld-arch arm64e \
  -o ~/Dev/mobile/ipsw/26.5.2-extracted \
  ~/Dev/mobile/ipsw/26.5.2/iPad17,1,iPad17,2,iPad17,3,iPad17,4_26.5.2_23F84_Restore.ipsw
# -> ~/Dev/mobile/ipsw/26.5.2-extracted/23F84__iPad17,1_2_3_4/dyld_shared_cache_arm64e{,.01..}
# (one .dyldlinkedit read failed under apfs-fuse; the executable sub-caches extracted fine.)

for f in .../dyld_shared_cache_arm64e .../dyld_shared_cache_arm64e.[0-9]*; do
  python3 tools/pacstrip/pacstrip.py scan "$f"
done
```

## Phase 2 status (the kernel build)

The next roadmap item is building UniXNU's xnu-12377 as a **plain arm64** kernel (no ptrauth). Findings:

- The 26.5.2 OS profile is `research/UniXNU/profiles/os/ipados-26.5.2-23F84` (xnu-12377.121.6, commit
  ac9718fb…); the plain-arm64 arch profile is `profiles/arch/arm64` (vs `arm64e`). The compile target is
  `DEVELOPMENT_ARM64_BCM2837` (4 KiB page), per the profile and `docs/guide/build-kernel.md`.
- `./unixnu build kernel` drives it, but `./unixnu defconfig` requires a **git-based SoC feed** for the
  board's `soc_source`. There is no `UniXNU-sm7325-common` yet — that is the Phase 3 SoC layer. A draft
  compile-only device is at `~/Dev/mobile/lisa-26-device` (board.toml + lisa26_defconfig, arm64 + 26.5.2,
  no SoC drivers); it stops at the missing `sm7325` soc feed.

### Next steps (Tuesday)

1. Get the kernel to COMPILE, one of two ways:
   - stand up a minimal `UniXNU-sm7325-common` git feed (even near-empty: configs/ + patches/xnu/series),
     wire it in `feeds.toml`, then `./unixnu fetch` + `./unixnu build kernel` in the docker toolchain; or
   - call `research/UniXNU/scripts/build-xnu-4k-baseline.sh` directly with `UNIXNU_XNU_TAG/COMMIT/DARWIN/
     VERSION` from the 26.5.2 profile and the `arch:patches/xnu/series`, bypassing the device/feed system.
2. Apply the darwin-lisa kernel patches: accept arm64e Mach-O (cpusubtype + ptrauth ABI), never touch the
   PAC key registers, and emulate **LDRAA/LDRAB/FJCVTZS** in the undefined-instruction handler.
3. Wire `kernel/pacstrip/pac_rewrite.c` as the `text_crypter` of the PAC-stripping pager (docs/pac-pager.md).
4. Boot under `qemu-system-aarch64 -cpu cortex-a76` to launchd, then SpringBoard.
