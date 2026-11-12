# cpufreq: the cores' clocks, and the governor

`hakios.cpufreq.c` (bsd/dev in the xnu-work tree; HakiOS's driver with darwin-lisa's additions) asks the EPSS, the
SoC's own frequency controller, for a row of each core group's frequency table: three groups on the SM7325 (four small
cores, three big, one prime), one register write per group. The boot loader leaves every group slow; `cpufreq=1`
(`lisa=1` sets it) raises them at boot.

## The governor (2026-11-11)

Before it every group sat at its top row for the whole boot, and the phone ran hot: the cores that do nothing (most of
them, most of the time) still ran at 1.8-2.4 GHz with the supply voltage the top row needs. Now, `cpugov_after`
seconds after the raise (the boot at full speed), a kernel thread looks every `cpugov_ms` at how busy each group's
busiest core was (`ml_cpu_idle_time`, the same share `cpuload=MS` prints as `tl: u`) and moves the row:

| share of the busiest core | the row |
|---|---|
| 95 % or more | the top row, at once |
| 80 % or more | up to the first row that would bring the share to 70 % (one row up at least), at once |
| 50 % to 80 % | as it is |
| under 50 % for three periods in a row | down to the row that would bring it to 70 %, never under the floor |

The floor is a third of the group's top clock unless `cpugov_floor=MHz` says otherwise: about 600 MHz on the small
cores, 800 on the big, 830 on the prime, so a touch after an idle minute is still answered by a core that is not at its
slowest. `cpufreq_max=MHz` still caps the top.

| boot-arg | default | meaning |
|---|---|---|
| `cpugov=0` | on | off: the top row for ever (every kernel before this) |
| `cpugov_after=S` | 90 | seconds after the raise before the first step down |
| `cpugov_ms=MS` | 100 | the period (20 at least) |
| `cpugov_floor=MHz` | top / 3 | the lowest clock any group is left at |
| `cpugov_log=1` | off | every change of row to the loader's log, at most one line every 2 s: `cpugov: group 1 row 7 -> 4, 1248 MHz, busy 23 %` |

Log markers: `cpugov: governing from now on: every 100 ms, floors 595/806/844 MHz` when it starts; `cpugov: off
(cpugov=0)` when not. With WFI idle (`wfi=1`, `kernel/patches/cpu-idle-no-wfi.patch` makes `wfi=0` the default) the
idle cores also stop clocking between interrupts, which is the other half of the heat; the governor is independent of
it.

Kernel: `kernel-16k-arm64e-gov.bak` (7e2d1223) is the first with it. Measured with `cpuload=5000 cpugov_log=1` in
host run gpu-ma3 (`research/hackios-xnu-kernel/logs/gpu-ma3-*`).

## The rest of the file

`cpuload=MS` (the `tl: u` / `tl: busy` / `tl: pac` lines), `membench=1|2`, `ddr=0|1|2` and the memory bandwidth vote
(`ddr_vote`, `ddr_rest`) are described at the top of their functions in `hakios.cpufreq.c`.
