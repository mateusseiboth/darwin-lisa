# Phase 2: emulating the un-rewritable arm64e instructions at EL0

Updated 2026-11-09. The PAC-stripping pager rewrites almost every arm64e instruction in place
(PAC* -> NOP, RETAB -> RET, BLRAA -> BLR, most LDRAA -> LDR/LDUR). Three forms have no 4-byte
replacement, so when userland executes one it traps on the A78, and the kernel emulates it. Phase 1's
scan of the real iPadOS 26.5.2 shared cache found exactly these, and only these:

| Instruction | Count | Why it cannot be rewritten |
|---|---:|---|
| `LDRAA` | 1,514 | authenticated load whose scaled offset is outside the LDUR/LDR window (W=0 & offset < -256, or W=1 & offset not in [-256,255]) |
| `LDRAB` | 1,021 | same, B key |
| `FJCVTZS` | 472 | FEAT_JSCVT (JavaScript double->int32); the A78 lacks the feature |

## The emulator core (host-tested)

`kernel/pacstrip/pac_emulate.c` (+`.h`) is pure and decodes with the same masks as
`tools/pacstrip/insn.py` and `pac_rewrite.c`, so it handles exactly the words those mark
`PAC_REWRITE_TRAP`. It touches no XNU types and reads memory through a callback, so it runs
unchanged in the kernel and in the host test (`test_pac_emulate.c`, `make test`).

- **Authentication is the identity.** In the rewritten world every PAC* became a NOP, so no pointer
  is ever signed. `LDRAA`/`LDRAB` are therefore a plain `base + scaled_offset` load, with optional
  writeback of the computed address, Rn==31 meaning SP, Rt==31 discarding the result.
- **FJCVTZS uses no FPU.** It decodes the IEEE-754 double from its integer fields (sign/exponent/
  mantissa), truncates toward zero, takes the result modulo 2^32 as a signed int32, and sets
  `PSTATE.<N,Z,C,V> = 0:Z:00` where Z marks an exact in-range integer. Not using the FPU matters:
  XNU kernel paths do not save/restore FP on their own. The test cross-checks the result against the
  host FPU over NaN / infinity / -0 / overflow / subnormal / fractional cases; the Z/exact bit is
  conservative (never claims exact when it is not), so a wrong guess can only cost JavaScriptCore a
  slow-path recompute, never a wrong value.

## The kernel glue

`kernel/pacstrip/pac_trap.c` (+`.h`) is the only XNU-aware file. `pac_emulate_user_trap(state, instr)`
marshals the trapped thread's saved state into a `pac_emulate_ctx` and writes the result back:

- GPRs/SP/PC/CPSR via `get`/`set_saved_state_*`; the NZCV field is `PSR64_NZCV_MASK`.
- `Dn` for FJCVTZS from `neon_state64(current_thread()->machine.uNeon)->v.d[n][0]`. This is current at
  the trap: an EL0 synchronous exception spills the whole NEON block to `uNeon` via `SPILL_REGISTERS`
  before any C runs.
- the `LDRAA`/`LDRAB` load goes through `copyin()`, so a bad user address faults instead of panicking.

It compiles clean against the real xnu-12377 headers (verified with `clang -fsyntax-only` using
sleh.o's own recorded flags).

## Wiring into the build

`kernel/patches/apply.sh <xnu-work>` (idempotent) copies the four sources into `osfmk/arm64/` and
applies two edit patches:

- `patches/sleh-pac-emulate.patch` — `handle_uncategorized()` calls `pac_emulate_user_trap()` for an
  EL0 undefined-instruction trap, before the generic exception path; a genuine kernel-mode or
  non-emulated trap still falls through unchanged (`pac_emulate_user_trap` returns false for them).
- `patches/conf-files-pac.patch` — lists `pac_emulate.c` and `pac_trap.c` in `osfmk/conf/files.arm64`.

The UniXNU 26.5.2 kernel then builds with the emulator linked in. Behavioural validation (an actual
trap taken and emulated) happens at the QEMU boot, once the rewritten arm64e userland is in place.

## Not handled here

- **The PAC key registers** need no work: the kernel is built plain arm64 (`HAS_APPLE_PAC` off), so
  XNU emits no key-register writes and no sign/auth code at all.
- **The kernel's own image**: its only PAC words are 176 XPAC-family (all `-> NOP`); `pacstrip strip`
  reduces the linked kernel to 0 PAC instructions, the same static path used for prelinked kexts.
- **LC_DATA_IN_CODE** inside a code section (a constant that looks like a PAC instruction): the pager
  leaves those ranges alone; Phase 1's cache scan bounds how common they are.
