# GPU + touch drivers (Goodix "Berlin" touch, Adreno 642L GPU)

Ported from HakiOS (xnu-8796) onto xnu-12377, beside the standard platform drivers.
Both are **optional** and **inert by default** (gated by a boot-arg), so a kernel that
carries them still boots exactly as before unless the arg is set.

## Sources
- `hakios.goodix.c`, `hakios.focaltech_fw.h` — Goodix touch + its FocalTech firmware blob.
  Copied unchanged (no KPI rebase needed).
- `hakios.adreno.c` — Adreno 642L GMU/GPU bring-up stepper. One KPI rebase: the KMA_*
  flags moved into `<vm/vm_kern_xnu.h>` in 26 (same move the audio driver needed).
- `hakios.adreno_glue.c` — file-local no-op stubs for the three metal-stack entry points
  (`HakiosGPUStart`/`HakiosGPUStop`/`MiniIOMobileFramebufferShow`). Created inline by
  `adreno-wire.py` (not vendored here). Delete it when the metal stack is ported, or its
  symbols clash with the real ones.

## Wiring (idempotent, run in docker as root, then `.work/run-rebuild.sh`)
- `goodix-wire.py`, `adreno-wire.py` (here); `../security/miniamfi-wire.py` for M1.
- Each adds the source under the `hakios_goodix` / `hakios_adreno` optional tag in
  `bsd/conf/files`, defines the matching `options HAKIOS_*` in `config/MASTER`, and adds
  the tag to `config/MASTER.arm64.bcm2837` (same mechanism the APFS port used). The
  standard `hakios.input.c` already chains `hakios_goodix_init()` / `hakios_adreno_init()`;
  enabling a tag swaps the `hakios.stubs.c` no-op for the real driver.

## Boot-args (loader cmdline)
- **Touch**: `touchscreen=1 touch_avdd_ldo=ldoc7` (AVDD defaults to 3000 mV). lisa's pins
  are already the compiled defaults (INT gpio81, RESET gpio105, SE13, I/O rail ldoc2);
  AVDD is the one lisa-specific value (PMIC ldoc7@3000, taken from the boot-arg). Optional:
  `touch_chip=goodix`, `touch_spi_hz=N`, `touch_lat=1`.
- **GPU**: `gpu=1` (full bring-up), `adreno=N` (step-by-step debug), or `display_probe=1`.

## Metal stack (deferred — needed for an actual framebuffer/Metal)
Port from HakiOS `iokit/Kernel/` (wired in `iokit/conf/files`): `hakios.gpu.cpp`
(IOGPU/IOUserClient), `MiniIOMobileFramebuffer.cpp`, `MiniIOSurface.cpp/.h`,
`hakios.stubs.cpp`. These are IOKit C++ (higher KPI churn 8796->12377 than the VFS/bsd
path). Runtime also needs the GPU firmware (`a660_gmu.bin`, `a660_sqe.fw`, `a660_zap.mdt`,
`a660_zap.b02`) in the RAM image and `HakiosMetal.bundle` in the root `/lib`. Adreno chip
id `0x06030500` (642L). When porting, delete `hakios.adreno_glue.c`.
