# ld64 wrappers for the UniXNU cross build

The cctools `ld64` in the toolchain image does not read `.tbd` text stubs (the
iPhoneOS SDK ships libSystem, libz, etc. as `.tbd`). These wrappers sit at
`xnu-deps/host/bin/ld64` in the HakiOS tree (gitignored there) and pick the right
linker:

- **ld64-dispatch** — the default for the userland build: static links (stage0,
  `/sbin/launchd`) go to cctools `ld`; dynamic links (`-lSystem`/`-dynamic`) go to
  lld, which reads `.tbd`. Point `ld64 -> ld64-dispatch` for `userland/build.sh`.
- **ld64-lld** — sends *everything* to lld's Mach-O flavor, translating/dropping the
  GNU/ELF linker flags lld rejects (`--whole-archive FOO.a` -> `-force_load FOO.a`;
  drop `--as-needed/--no-undefined/--start-group/--end-group/--gc-sections` and
  `-soname`). Point `ld64 -> ld64-lld` for the Mesa build (`tools/mk-mesa.sh`), whose
  meson link tests pass neither `-lSystem` nor `-dynamic`.

Kernel builds use cctools: `ld64 -> /opt/unixnu-linux/CommandLineTools/usr/bin/ld`.
