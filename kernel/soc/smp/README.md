# Several processors on lisa (xnu-12377)

Port of HakiOS's `GENERIC_PSCI_SMP` (xnu-8796, `osfmk/arm64/psci_smp.c` + `GenericARMCPU`), which ran iOS 16
on this phone with `cpus=8`, to the iOS 26 kernel.

## What forced one core

1. **No CPU driver registered any core.** The base platform is BCM2837 (no `USE_APPLEARMSMP`, no Apple CPU kext).
   UniXNU's `ArmCpu` is linked in, but it wants `/arm-io/intc` and a `/psci` node, which the lisa loader does not
   write (it writes `/arm-io/gic` and `/chosen/psci-method`), so it never started, and its secondary cores are
   opt-in anyway (`sheng-smp=1`). Nothing called `ml_processor_register()`, so nothing called `ml_set_max_cpus()`
   (the reason for the `startup.c` bring-up fix) and no secondary core was ever started.
2. **The reset vector could not find a lisa core's cpu data.** Without `HAS_CLUSTER`, `start.s` matched
   `MPIDR & 0xFF`; lisa numbers its cores in Aff1 (`0x100` ... `0x700`) with Aff0 zero, so every core read as 0.
3. **No vectors for a PSCI-started core.** The stock reset path writes `OSLAR_EL1` before it has a `VBAR_EL1`, and
   the low vectors (`b .`) hide any early fault.
4. **No interrupts between processors.** Only the virtual timer (PPI 27) was routed to EL1.

## The pieces

| File (darwin-lisa) | Lands in xnu-work | What it does |
| --- | --- | --- |
| `soc/smp/lisa_smp.c` | `osfmk/arm64/lisa_smp.c` | PSCI `CPU_ON`/`CPU_OFF` (SMC or HVC, from `/chosen/psci-method`), cleans the kernel segments and every cpu data to PoC before `CPU_ON`, waits 5 s for the core and logs what became of it (`lisa-smp: cpu N ... is up`), SGIs via `ICC_SGI1R_EL1` |
| `soc/smp/LisaCPU.cpp` | `iokit/Kernel/LisaCPU.cpp` | IOCPU driver for the `/cpus` nubs ArmDTPlatform publishes. Registers the boot core first (secondaries that match earlier wait), sends each core its first SGI 0 from `initCPU()` (clears `SIGPdisabled`) |
| `soc/smp/smp-wire.py` | `KernelConfigTables.cpp`, `start.s`, `cpu.c`, conf files | LisaCPU personality (score 3000, above ArmCpu's 2000); `reset_vector` with its own vector table (`LowLisaSecondaryVectorBase`, which records a fault in `lisa_smp_start_fault` and turns the core off), a sane `SCTLR_EL1`, the `Aff1|Aff0` match; no set/way cache flush at every idle on BCM2837 |
| `soc/gic_vtimer.c` | `osfmk/arm64/gic_vtimer.c` | SGI 0 -> `cpu_signal_handler()`; the heartbeat lines are the boot core's |
| `patches/pe_fiq-vtimer-group1.patch` | `pexpert/arm/pe_fiq.c` | SGI 0 in Group 1, priority 0xA0, enabled, stale pending/active cleared, on every core |
| `loader-console/loader_console.c` | `osfmk/arm64/loader_console.c` | watchdog pet, heartbeat and timed return only on the boot core; the 50 ms timer floor on every core |

On xnu-12377, `ml_processor_register()` boots the processor itself (`processor_boot()`): the boot core runs
`cpu_machine_init()` -> `initCPU(true)` at once; a secondary goes `processor_dostartup()` -> `cpu_start()` ->
`LisaCPU::startCPU()` -> `CPU_ON`, and the new core comes to `initCPU()` from `processor_cpu_reinit()`.

## Use

Inert with one core: `LisaCPU::probe()` declines when the topology has one CPU, so `cpus=1` (what the loader
prepends by default) boots exactly as before. Put `cpus=N` in the kernel arguments (the loader then does not
prepend `cpus=1`). With `bootcpu=7` the loader lists the cores that are off in the order 7, 6, 5, 4 (Cortex-A78)
then 3, 2, 1 (Cortex-A55), and stays on core 0 with the USB log and its watchdog petting. So `cpus=4` is the four
big cores, `cpus=7` all the kernel can have.

## Build

```
docker run --rm -v ~/Dev/mobile:/home/mateusseiboth/Dev/mobile darwin-lisa/unixnu-toolchain:latest \
  python3 /home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/soc/smp/smp-wire.py
darwin-lisa/.work/run-rebuild.sh        # re-applies kernel/patches (pe_fiq, gic_vtimer.c, loader_console.c)
```

`XNU_WORK=<dir>` points the wire script at another tree (a dry run on a copy).

## On the device (2026-11-10)

Full iOS 26 boot (trimmed USB image, `run-fullroot.sh`), `cpufreq=1 cpufreq_max=2400`, `wfi=0` (the default since
`cpu-idle-no-wfi.patch` is in `apply.sh`). Times are from the kernel's first line (`Darwin Kernel Version`, ~103.85 s
on the counter) to launchd's first log line and to the log line around backboardd's first swap
(`MiniIOMobileFramebuffer: swap requests of ...`).

| Run | Cores | Kernel start -> launchd | -> first swap | Stable | Log (research/hackios-xnu-kernel/logs) |
| --- | --- | --- | --- | --- | --- |
| baseline (`kernel-16k-sbwip.bak`) | 1 @ firmware clock | +4.2 s | ~+50 s | - | `sb-run20.log` |
| cpufreq only | 1 @ 2400 MHz | +2.0 s | +21 s | panic at 14.6 min (framebuffer bzero, fixed) | `cpu-a1-wfi0-2400*.log` |
| `cpus=4` | 7, 6, 5, 4 @ 2400 | +1.9 s | +8.7 s | 20.3 min, no panic, lock screen ("Swipe up to open") | `cpu-b1-smp4.log`, `cpu-b1-smp4-shots/` |
| `cpus=7` | 7..4 @ 2400, 3..1 @ 1804 | +2.2 s | +8.6 s | 20.4 min, no panic, panel drawn; vnode-jetsam storm (~1000 kills) | `cpu-b2-smp7.log`, `cpu-b2-smp7-shots/` |
| `cpus=7` + `maxvnodes-wire.py` (`kernel-16k-smp-vnodes.bak`) | same | +2.6 s | +5.1 s | 20.5 min, no panic, 0 jetsam kills, SpringBoard cover sheet ("Swipe up to open") by ~4 min, cores 63-72% busy | `cpu-b3-smp7-vnodes.log`, `cpu-b3-smp7-vnodes-shots/` |

Every core takes work: `cpuload=5000` prints each core's busy share every 5 s
(`tl: u at 114805428 7:56 6:52 5:58 4:37 3:45 2:49 1:51`, then 100% on all seven). The 100% was a jetsam storm, not
real work: with 1024 vnodes (BCM2837 "xsmall") the table is full within minutes and memorystatus kills daemons for
vnodes (~1000 kills in 20 min) that launchd restarts at once. `kernel/pg16k/maxvnodes-wire.py` sizes it from memory.
Single-core boots never got that far fast enough to show it.

Open: idle cores poll (`wfi=0`), which keeps them hot. Whether an SGI wakes a core parked in WFI under this
hypervisor (it should: HakiOS ran `cpus=8` with WFI) is not measured yet; `wfi=1` brings WFI back for that test.
