# Darwin on the Xiaomi 11 Lite 5G NE (lisa)

Research port: Apple's open-source XNU and an iOS 26 user space on the Xiaomi 11 Lite 5G NE (`lisa`, Snapdragon 778G
SM7325). Nothing of Apple's is in this repository. The user space comes from an IPSW you get yourself, and the phone's
firmware comes from your own phone.

The target is **iOS 26 or newer** on a CPU **without pointer authentication** (Cortex-A78 and A55, ARMv8.2). Nobody has
published this combination yet. [ROADMAP.md](ROADMAP.md) explains why it is hard and how this port gets there.

## What is here

| Path | What |
| --- | --- |
| [`tools/pacstrip/`](tools/pacstrip) | Rewrites arm64e code for a CPU without pointer authentication. Our own implementation, from the Arm ARM encodings, checked against llvm-mc |
| [`kernel/pacstrip/`](kernel/pacstrip) | The same rewrite in C, for the kernel's page-in path. `make test` checks it against vectors made by the Python version |
| [`docker/unixnu-toolchain/`](docker/unixnu-toolchain) | UniXNU's Linux toolchain for XNU, built inside a Docker image |
| [`docs/pac-pager.md`](docs/pac-pager.md) | How the kernel rewrites arm64e code pages on page-in, and where in XNU |
| [ROADMAP.md](ROADMAP.md) | The plan, phase by phase, and what each phase is built on |

## pacstrip

```sh
tools/pacstrip/pacstrip.py scan FILE...     # count what would change; list what the kernel must emulate
tools/pacstrip/pacstrip.py strip IN [OUT]   # rewrite IN into OUT (in place without OUT)
```

| Instruction | Becomes |
| --- | --- |
| `RETAA`, `RETAB` | `RET` |
| `BRAA`, `BRAB`, `BRAAZ`, `BRABZ` | `BR` with the same target register |
| `BLRAA`, `BLRAB`, `BLRAAZ`, `BLRABZ` | `BLR` with the same target register |
| `ERETAA`, `ERETAB` | `ERET` |
| `PAC*`, `AUT*`, `XPAC*` (register forms) | `NOP` |
| `PACGA Xd, Xn, Xm` | `EOR Xd, Xn, Xm, ROR #13` (deterministic, which is all its users need) |
| `LDRAA`, `LDRAB` | `LDR` (unsigned offset), `LDUR` (offset -256 to -8) or `LDR` pre-indexed (writeback, -256 to 248) |
| `LDRAA`/`LDRAB` out of those ranges, `FJCVTZS` | left in place and reported: the kernel emulates them |
| hint forms (`PACIASP`, `AUTIBSP`, ...) | left in place: they already execute as `NOP` |

Only arm64 slices of thin and fat files are touched, only in sections marked as code, and never inside the ranges
`LC_DATA_IN_CODE` lists. The code signature is not refreshed.

`tools/pacstrip/featscan.py FILE` lists the instructions that the Apple A13 (the oldest iOS 26 CPU) decodes and the
Cortex-A78 does not, with their counts, using LLVM's disassembler for both CPUs (`--have` and `--want` take any
`-mcpu` LLVM knows). Run it on the rewritten files: what it prints is what the kernel has to emulate.

Tests: `cd tools/pacstrip && python3 -m unittest`. The exhaustive tests generate every encoding of the rewritten families
and check each rewrite against llvm-mc's disassembler. One test compiles C with `clang -target arm64e-apple-ios17.0` and
checks that nothing authenticated is left.

## License

MIT.
