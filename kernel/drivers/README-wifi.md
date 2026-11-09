# Wi-Fi driver (Qualcomm WCN6750, the Wi-Fi of the SM7325 "lisa")

Ported from HakiOS (xnu-8796) onto xnu-12377, beside the standard platform drivers and the
Goodix/Adreno ports. Like them it is **optional** and **inert by default** (gated by the
`hakios_wifi` driver tag + the `wifi` boot-arg): a kernel that carries it boots exactly as
before unless `wifi=1` (or `wifi_step=N`) is set. `hakios_wifi_init()` returns at its first
line without the boot-arg.

## Sources (vendored here; `wifi-wire.py` copies them into the xnu-work tree)
bsd side (all `optional hakios_wifi` in `bsd/conf/files`):
- `hakios.wifi.c` — the main driver: starts the WPSS DSP that runs the radio's firmware, brings
  up the GLINK/QRTR links, steps through the bring-up (`wifi_step=N`). KPI rebase: `vm_kern_xnu.h`.
- `hakios.qmi.c` — QMI messaging over the IPC router. The HakiOS Wi-Fi driver `requires QMI`;
  wifi is the only consumer we enable, so it rides the same `hakios_wifi` tag. Guarded by
  `#ifdef KERNEL` (its userland test half is excluded in-kernel).
- `hakios.wlfw.c` — the firmware QMI exchange (WLFW service). See "Deferred" below: it compiles
  to its built-in stub branch because ath11k's message tables are not generated in this tree.
- `hakios.lx.c` — the ath11k-shaped net glue (interface attach, ring poll, IRQ helpers). KPI
  rebase: `vm_kern_xnu.h`.
- `hakios.sta.c`, `hakios.apple80211.c` — the station and the Apple80211 ioctl surface wifid uses.
- `hakios.wifi_glue.c` — file-local GICv3-SPI stubs (see "Deferred"). Created by `wifi-wire.py`
  (not vendored); delete it when the real SPI routing is ported.

bsd headers (`bsd/sys/`): `hakios.lx.h`, `hakios.qmi.h`, `hakios.qrtr.h`, `hakios.sta.h`.
(`hakios.net.h`/`hakios.input.h` were already in the tree from the net/input port.)

iokit side (`optional hakios_wifi iokitcpp` in `iokit/conf/files`):
- `hakios.wlan.cpp` — `HakiosWLANStart`: the registry objects Darwin's wifid + configd look for
  (an `IO80211Controller`/`IOEthernetController`/`IONetworkController` under the `wlan` node, with
  the radio's `local-mac-address`, and an `IONetworkInterface` carrying wifid's properties). These
  are the file's own minimal `IOService` subclasses, not Apple's IO80211Family, so the KPI churn
  8796->12377 is nil (only stable `IOService`/`IOUserClient`/`OSData`/`OSNumber` KPI).
- `MiniIONetworkStack.cpp` + `MiniDrivers.h` — the `IONetworkStack` stand-in configd's interface
  namer waits for (QUIET/TIMEOUT). HakiOS starts it from its own `GenericARMPE`; this tree keeps
  Apple's platform expert, so `wifi-wire.py` adds one line to `hakios.wlan.cpp` that calls
  `MiniIONetworkStackStart()` from `HakiosWLANStart` (the one wifi-gated moment the interface is
  published). So both iokit objects stay inert until a Wi-Fi interface actually comes up.

The standard `hakios.input.c` already chains `hakios_wifi_init()`; enabling the tag swaps the
`hakios.stubs.c` no-ops (`#ifndef HAKIOS_WIFI`: `hakios_wifi_init`/`_stop`/`_quiesce`,
`hakios_apple80211_ioctl`, `HakiosWLANStart`) for the real driver.

## Wiring (idempotent, run in docker as root, then `.work/run-rebuild.sh`)
`.work/wifi-wire.py` copies the sources, applies the KPI rebases + the two `hakios.wlan.cpp`
edits, writes `hakios.wifi_glue.c`, lists the sources under `hakios_wifi` in `bsd/conf/files` and
`iokit/conf/files`, defines `options HAKIOS_WIFI` in `config/MASTER`, and adds `hakios_wifi` to
`config/MASTER.arm64.bcm2837` (BSD_DEV) — the same spot and mechanism goodix/adreno used.

## KPI rebase done (8796 -> 12377)
- `KMA_KOBJECT`/`KMA_ZERO`/`KMA_DATA` moved from `<vm/vm_kern.h>` into `<vm/vm_kern_xnu.h>` in 26.
  Added that include to the two sources that allocate with `kmem_alloc`/`kmem_alloc_contig`
  (`hakios.wifi.c`, `hakios.lx.c`); the others use `kalloc_data` only. Same move adreno.c needed.
- No `vm_map_copy_overwrite` / MTE-signature churn in the Wi-Fi sources.

## Boot-args (loader cmdline)
- `wifi=1` — full bring-up beside the system's start (a worker thread runs the steps). Needs the
  firmware in `fw-wifi/` (below); without it the driver prints the "no Wi-Fi" line and stops.
- `wifi_step=N` — run steps 1..N and stop (bring-up debugging). `wifi_mac=XX:XX:XX:XX:XX:XX` gives
  the radio's address. Many sub-args (`wifi_loud`, `wifi_pause`, `wifi_mission`, `wifi_cal`,
  `wifi_smmu`, `wifi_irq`, `wifi_join`, `wifi_keep`, ...) are documented in `hakios.wifi.c`'s head.
- No arg => the driver is inert (early return in `hakios_wifi_init`).

## Required firmware (I provide these from the phone's /vendor/firmware)
Read with `hakios_image_file("fw", name, ...)` (hakios.image.c), i.e. from the **`fw-wifi/`**
directory of the small RAM image, exactly like the Adreno `a660_*` blobs. Put the phone's
`/vendor/firmware_mnt/image/` files into `fw-wifi/`:
- `wpss.mdt` and `wpss.b02` ... `wpss.b07` — the WPSS DSP firmware (the metadata header + the
  loadable segments). `wpss.mdt` is mandatory for `wifi=1`.
- `bd_m20gl.elf` — the board's own data file (ath11k asks for `board.bin`/`board-2.bin`; the
  driver serves this).
- `regdb_xiaomi.bin` — the regulatory database (ath11k asks for `regdb.bin`).
- `network` (optional) — SSID + passphrase to join at start (`wifi_join=1` / `wifi=1`); `fw-wifi/network`.
- `mac` (optional) — `xl` reads `fw-wifi/mac` and passes it as `wifi_mac=`; this is the radio's address.

## First-boot log markers (what to watch for)
All lines are `printf`/`tell` prefixed `wifi:`, teed into the loader_log ring.
- Start: `wifi: the radio is being brought up beside the system's start (wifi=1)` — init launched
  the worker thread (driver is live, inert check passed).
- Progress: `wifi: step N: done` for each bring-up step; firmware answers as
  `wifi: N: THE FIRMWARE INDICATES "<...>"`.
- Interface publish: `HakiosWLAN: ...` registry lines + `MiniIONetworkStack: opened by <proc>; the
  registry's busy state is <n>` (configd's namer found the stack) when en0 attaches.
- Expected non-fatal: at step 15 (WLFW exchange) `wifi: 15: THE EXCHANGE IS NOT BUILT IN: ath11k's
  message descriptions were not there when this kernel was built (tools/mk-wlfw.sh, then build
  again)` — this is the deferred ath11k piece, not a failure.
- Failure to watch: `wifi: wifi=1, but the radio's firmware is not in the small image (fw-wifi/,
  see the README): no Wi-Fi` (firmware missing); `wifi: ...this is not the phone` (SOC base
  mismatch — should not happen on device).

## Deferred (what the real end-to-end needs that this pass does not do)
1. **ath11k WLFW message tables** — `hakios.wlfw.c` guards the real QMI firmware exchange behind
   `#if !__has_include("hakios.wlfw_ath11k.h")`. That header is generated by HakiOS
   `tools/mk-wlfw.sh` from ath11k's BSD-3-Clause-Clear `qmi.c`/`qmi.h` (fetched by `mk-ath11k.sh`),
   and is not kept in any repo. Without it the file compiles to its stub branch and step 15 reports
   "not built in". To get the real firmware handshake, generate the header into the kernel build dir
   and rebuild.
2. **GICv3 SPI routing** — `hakios.lx.c`'s `lx_os_irq_*` (the radio's hardware-interrupt path,
   `wifi_irq=1`) calls `gic_v3_spi_attach/mask/unmask/detach/count`, which HakiOS defines in
   `osfmk/arm64/gic_v3_irq.c` (a full GICv3 SPI driver). This tree carries only the vtimer/FIQ
   GICv3 bring-up (not to be touched), so those symbols are stubbed in `hakios.wifi_glue.c`. They
   are never called at runtime here: `lx_os_irq_*` is reached only from ath11k's `lx_bus.c`, which
   is not linked, and the default Wi-Fi path polls the radio's rings every millisecond. Porting
   `gic_v3_irq.c` (the radio's MSI through its SMMU context to the GIC) is what real interrupts
   need; delete `hakios.wifi_glue.c` then.
3. **ath11k data path (`lx_bus.c`/`lx_80211.c`)** — the ath11k-derived bus/80211 objects are a
   separate static lib (`tools/mk-ath11k.sh`), not part of this kernel. The ported `hakios.lx.c`
   is the OS-side glue; the actual 802.11 data path is the ath11k object, still to be linked in.
4. **Userland** — the supplicant/wifid/configd side runs from the real iOS root; this pass is the
   kernel side only. `HakiosWLANStart` + `MiniIONetworkStack` publish the registry objects they
   wait on.
