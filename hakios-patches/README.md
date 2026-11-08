# HakiOS (oosexgang/hackios-xnu-kernel) patches for the lisa port

These are the changes to the shared HakiOS tree (which lives upstream at
`github.com/oosexgang/hackios-xnu-kernel`, not here). Apply on top of its `main`:

    git clone https://github.com/oosexgang/hackios-xnu-kernel && cd hackios-xnu-kernel
    git am ../darwin-lisa/hakios-patches/lisa-port-shared-changes.patch

Contents:
- **Goodix touch clock HWCG fix** (drivers/input/hakios.goodix.c) — proven on lisa.
- **mk-mesa: find the SDK's zlib through pkg-config** (tools/mk-mesa.sh) — part of the
  GL=1 toolchain fix; with the ld64 wrappers (../docker/unixnu-toolchain/ld64-wrappers)
  Mesa 25.0.7 (freedreno) builds and the Adreno 642L renders the iOS home.
- device packaging + capybara/6BOT branding.

The lisa device repository itself is the `lisa-device` branch of this repo.
The uncommitted touch SPI/firmware work-in-progress (4-dummy gt9897 protocol, reset,
bit-bang diagnostics) is not here yet.
