/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X5 Pro (redwood).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 */
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <sys/bsdtask_info.h>	/* bsd_getthreadname */
#include <sys/param.h>
#include <sys/proc.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>   /* darwin-lisa: kmem_alloc and the KMA_* flags on xnu-12377 */

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (hakios_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define EPSS_PHYS               0x18591000UL    /* three groups ("domains") of 0x1000: small cores, big, biggest */
#define EPSS_DOMAIN_SIZE        0x1000UL
#define EPSS_DOMAINS            3
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/* The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second field). */
static uint32_t
domain_of(uint32_t phys_id)
{
	uint32_t core = (phys_id >> 8) & 0xff;

	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

static uint32_t cpufreq_max_mhz;

/*
 * darwin-lisa: what each group's frequency table says, kept for the governor below: the rows this kernel may ask for
 * (up to cpufreq_max), the one it asks for now, and the floor it never goes under.
 */
static struct cpufreq_group {
	vm_offset_t base;
	uint32_t rows;                          /* rows 0 .. rows - 1 are usable, ascending */
	uint32_t khz[EPSS_TABLE_ROWS];
	uint32_t row;                           /* the row asked for now */
	uint32_t floor;                         /* the governor's lowest row */
	uint32_t quiet;                         /* periods in a row below the step-down share */
	bool ours;                              /* a core of ours is in this group */
} cpufreq_groups[EPSS_DOMAINS];

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	struct cpufreq_group *g = &cpufreq_groups[domain];
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		/* darwin-lisa: boot-arg cpufreq_max=MHz, the fastest row not above it (the top row ran hot on lisa) */
		if (row > 0 && cpufreq_max_mhz != 0 && khz / 1000 > cpufreq_max_mhz) {
			break;
		}
		top = row;
		top_khz = khz;
		g->khz[row] = khz;
	}
	g->base = base;
	g->rows = top + 1;
	g->row = top;
	g->ours = true;
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
}

/*
 * darwin-lisa: the governor. The top row of every group all the time is what made the phone hot: the cores that do
 * nothing (most of them, most of the time) still run at 1.8-2.4 GHz, and the EPSS raises the supply voltage with the
 * clock. Here, once the boot is done, each group's row follows the busiest of its cores (as the display's own clock
 * would, cpuload=MS: tl: u) -- up at once, down only after a few quiet periods -- and never under a floor, so that the
 * first touch after an idle minute is still answered by a core that is not at its slowest.
 *
 *   cpugov=0           off: the top row for ever (what every kernel before this did)
 *   cpugov_after=S     seconds after this starts before the first step down (default 90: the boot at full speed)
 *   cpugov_ms=MS       the period (default 100)
 *   cpugov_floor=MHz   the lowest clock any group is left at (default: a third of the group's top, so about
 *                      600 MHz small, 800 big, 830 prime)
 *   cpugov_log=1       every change of row to the loader's log ("cpugov: group 1 row 7 -> 4, 1248 MHz, busy 23 %")
 *
 * The rule, per group and period: the share is the busiest core's. 95 % or more: the top row. 80 % or more: up to the
 * first row that would bring the share to 70 % (one row at least). Under 50 % for three periods: down to the row that
 * would bring it to 70 %, never under the floor. Between: as it is.
 */
static uint32_t gov_after_s = 90, gov_period_ms = 100, gov_floor_mhz, gov_log;

static void
gov_set_row(struct cpufreq_group *g, unsigned domain, uint32_t row, uint32_t busy)
{
	static uint64_t last_line;
	uint64_t now, since;

	if (row == g->row) {
		return;
	}
	*(volatile uint32_t *)(g->base + EPSS_PERF_STATE) = row;
	now = mach_absolute_time();
	absolutetime_to_nanoseconds(now - last_line, &since);
	if (gov_log != 0 && (last_line == 0 || since > 2 * NSEC_PER_SEC)) {
		last_line = now;
		tell("cpugov: group %u row %u -> %u, %u MHz, busy %u %%\n", domain, g->row, row, g->khz[row] / 1000, busy);
	}
	g->row = row;
}

/* the lowest row at or above KHZ, within floor .. top */
static uint32_t
gov_row_for(const struct cpufreq_group *g, uint64_t khz)
{
	uint32_t row = g->floor;

	while (row + 1 < g->rows && g->khz[row] < khz) {
		row++;
	}
	return row;
}

static void
gov_decide(unsigned domain, uint32_t busy)
{
	struct cpufreq_group *g = &cpufreq_groups[domain];
	uint64_t want = (uint64_t)g->khz[g->row] * busy / 70;   /* the clock at which this share would be 70 % */
	uint32_t top = g->rows - 1, row;

	if (busy >= 95) {
		g->quiet = 0;
		gov_set_row(g, domain, top, busy);
		return;
	}
	if (busy >= 80) {
		g->quiet = 0;
		row = gov_row_for(g, want);
		gov_set_row(g, domain, MAX(row, MIN(g->row + 1, top)), busy);
		return;
	}
	if (busy >= 50) {
		g->quiet = 0;
		return;
	}
	if (++g->quiet < 3) {
		return;
	}
	g->quiet = 0;
	row = gov_row_for(g, want);
	gov_set_row(g, domain, MIN(row, g->row), busy);
}

static void
gov_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then;

	delay_for_interval(gov_after_s * 1000, NSEC_PER_MSEC);
	then = mach_absolute_time();
	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		(void)ml_cpu_idle_time((int)i, &idle_before[i]);
	}
	tell("cpugov: governing from now on: every %u ms, floors %u/%u/%u MHz\n", gov_period_ms,
	    cpufreq_groups[0].khz[cpufreq_groups[0].floor] / 1000, cpufreq_groups[1].khz[cpufreq_groups[1].floor] / 1000,
	    cpufreq_groups[2].khz[cpufreq_groups[2].floor] / 1000);
	for (;;) {
		uint32_t busy[EPSS_DOMAINS] = { 0, 0, 0 };
		uint64_t now;

		delay_for_interval(gov_period_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, elapsed = now - then, slept, share;
			unsigned domain = domain_of(topology->cpus[i].phys_id);

			if (!ml_cpu_idle_time((int)i, &idle) || elapsed == 0) {
				continue;
			}
			slept = idle >= idle_before[i] ? idle - idle_before[i] : 0;
			idle_before[i] = idle;
			share = slept >= elapsed ? 0 : 100 * (elapsed - slept) / elapsed;
			busy[domain] = MAX(busy[domain], (uint32_t)share);
		}
		then = now;
		for (unsigned domain = 0; domain < EPSS_DOMAINS; domain++) {
			if (cpufreq_groups[domain].ours && cpufreq_groups[domain].rows > 1) {
				gov_decide(domain, busy[domain]);
			}
		}
	}
}

static void
gov_start(void)
{
	uint32_t on = 1;
	thread_t thread;

	PE_parse_boot_argn("cpugov", &on, sizeof(on));
	if (on == 0) {
		tell("cpugov: off (cpugov=0): every group stays at its top row\n");
		return;
	}
	PE_parse_boot_argn("cpugov_after", &gov_after_s, sizeof(gov_after_s));
	PE_parse_boot_argn("cpugov_ms", &gov_period_ms, sizeof(gov_period_ms));
	PE_parse_boot_argn("cpugov_floor", &gov_floor_mhz, sizeof(gov_floor_mhz));
	PE_parse_boot_argn("cpugov_log", &gov_log, sizeof(gov_log));
	gov_period_ms = MAX(gov_period_ms, 20);
	for (unsigned domain = 0; domain < EPSS_DOMAINS; domain++) {
		struct cpufreq_group *g = &cpufreq_groups[domain];
		uint32_t floor_khz;

		if (!g->ours || g->rows == 0) {
			continue;
		}
		floor_khz = gov_floor_mhz != 0 ? gov_floor_mhz * 1000 : g->khz[g->rows - 1] / 3;
		g->floor = 0;
		while (g->floor + 1 < g->rows && g->khz[g->floor] < floor_khz) {
			g->floor++;
		}
	}
	if (kernel_thread_start(gov_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.ready, relaxed);
		os_atomic_inc(&bench.done, relaxed);
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	os_atomic_inc(&bench.ready, relaxed);
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */
	uint64_t total[3] = { 0, 0, 0 };

	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.ready, relaxed);
			os_atomic_inc(&bench.done, relaxed);
		}
	}
	while (os_atomic_load(&bench.ready, relaxed) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	os_atomic_store(&bench.go, 1, release);
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		tell("membench: all at once: core %u: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", bench.core[i],
		    bench.ns[i][0] ? mb * 1000000000ull / bench.ns[i][0] : 0, bench.ns[i][1] ? mb * 1000000000ull / bench.ns[i][1] : 0,
		    bench.ns[i][2] ? mb * 1000000000ull / bench.ns[i][2] : 0);
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", threads, total[0],
	    total[1], total[2]);
}

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

/*
 * darwin-lisa: with the shares, WHO was running on a core that was more than half busy (the task's name and the
 * thread's id and name, "tl: busy 6:SpringBoard/123(main)"), and the PAC emulator's counts since the last line, by
 * task ("tl: pac +N SpringBoard:N backboardd:N"; osfmk/arm64/pac_trap.c): a process with PAC instructions left in
 * its hot path traps on every one, and that is what a core at 100 % for half a minute after a swipe looked like.
 */
#define DLISA_PAC_TASKS 8
struct dlisa_pac_count { int pid; uint64_t count; };
extern struct dlisa_pac_count dlisa_pac_counts[DLISA_PAC_TASKS];
extern uint64_t dlisa_pac_total;
#define DLISA_PAC_PCS 32
struct dlisa_pac_pc { int pid; uint64_t pc; uint64_t count; };
extern struct dlisa_pac_pc dlisa_pac_pcs[DLISA_PAC_PCS];

/* the six instruction addresses that trapped most since the last line: "tl: pacpc SpringBoard:0x1a2b3c4d8:1234 ..." */
static void
load_pac_pcs(char *line, size_t size)
{
	static uint64_t before[DLISA_PAC_PCS], pc_before[DLISA_PAC_PCS];
	uint64_t delta[DLISA_PAC_PCS];
	int at = snprintf(line, size, "tl: pacpc");
	unsigned shown = 0;

	for (unsigned i = 0; i < DLISA_PAC_PCS; i++) {
		uint64_t count = dlisa_pac_pcs[i].count;

		delta[i] = dlisa_pac_pcs[i].pc == pc_before[i] && count >= before[i] ? count - before[i] : count;
		pc_before[i] = dlisa_pac_pcs[i].pc;
		before[i] = count;
	}
	for (; shown < 6 && at < (int)size - 60; shown++) {
		unsigned top = 0;
		char name[2 * MAXCOMLEN + 1] = "?";

		for (unsigned i = 1; i < DLISA_PAC_PCS; i++) {
			if (delta[i] > delta[top]) {
				top = i;
			}
		}
		if (delta[top] == 0) {
			break;
		}
		proc_name(dlisa_pac_pcs[top].pid, name, sizeof(name));
		at += snprintf(line + at, size - (size_t)at, " %s:%#llx:%llu", name, (unsigned long long)dlisa_pac_pcs[top].pc,
		    (unsigned long long)delta[top]);
		delta[top] = 0;
	}
	if (shown == 0) {
		line[0] = 0;
	}
}

static void
load_who(const ml_topology_info_t *topology, unsigned i, char *line, size_t size, int *at)
{
	extern thread_t dlisa_cpu_active_thread(unsigned cpu);   /* osfmk/arm64/pac_trap.c */
	extern int dlisa_thread_sched_pri(thread_t thread, bool *rt);  /* osfmk/kern/sched_prim.c (sched-prefer-select.patch) */
	thread_t thread = dlisa_cpu_active_thread(topology->cpus[i].cpu_id);
	task_t task;
	char name[2 * MAXCOMLEN + 1] = "?", tname[64] = "";
	void *uth;
	bool rt = false;
	int pri;

	if (thread == THREAD_NULL || *at >= (int)size - 48) {
		return;
	}
	task = get_threadtask(thread);
	if (task != TASK_NULL) {
		proc_name(task_pid(task), name, sizeof(name));
	}
	if ((uth = get_bsdthread_info(thread)) != NULL) {
		bsd_getthreadname(uth, tname);
	}
	pri = dlisa_thread_sched_pri(thread, &rt);
	*at += snprintf(line + *at, size - (size_t)*at, " %u:%s/%llu(%s)p%d%s", (topology->cpus[i].phys_id >> 8) & 0xff, name,
	    (unsigned long long)thread_tid(thread), tname, pri, rt ? "rt" : "");
}

static void
load_pac(char *line, size_t size)
{
	static uint64_t total_before, before[DLISA_PAC_TASKS];
	static int pid_before[DLISA_PAC_TASKS];
	int at;

	if (dlisa_pac_total == total_before) {
		line[0] = 0;
		return;
	}
	at = snprintf(line, size, "tl: pac +%llu", (unsigned long long)(dlisa_pac_total - total_before));
	total_before = dlisa_pac_total;
	for (unsigned i = 0; i < DLISA_PAC_TASKS && at < (int)size - 40; i++) {
		char name[2 * MAXCOMLEN + 1] = "?";
		uint64_t count = dlisa_pac_counts[i].count, delta = dlisa_pac_counts[i].pid == pid_before[i] && count >= before[i] ? count - before[i] : count;

		pid_before[i] = dlisa_pac_counts[i].pid;
		before[i] = count;
		if (delta == 0) {
			continue;
		}
		proc_name(dlisa_pac_counts[i].pid, name, sizeof(name));
		at += snprintf(line + at, size - (size_t)at, " %s:%llu", name, (unsigned long long)delta);
	}
}

/* osfmk/kern/sched_prim.c (sched-prefer-select.patch): "tl: sp held N late N hand N move N pull N" since the last line */
extern void dlisa_sched_prefer_line(char *line, size_t size);

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192], who[256], pac[256];
		int at, who_at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		who_at = snprintf(who, sizeof(who), "tl: busy");
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy, share;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			share = 100 * busy / (now - then);
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff, share);
			}
			if (share >= 50) {
				load_who(topology, i, who, sizeof(who), &who_at);
			}
		}
		then = now;
		hakios_tl("%s\n", line);
		if (who_at > (int)strlen("tl: busy")) {
			hakios_tl("%s\n", who);
		}
		dlisa_sched_prefer_line(pac, sizeof(pac));
		if (pac[0] != 0) {
			hakios_tl("%s\n", pac);
		}
		load_pac(pac, sizeof(pac));
		if (pac[0] != 0) {
			hakios_tl("%s\n", pac);
			load_pac_pcs(pac, sizeof(pac));
			if (pac[0] != 0) {
				hakios_tl("%s\n", pac);
			}
		}
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			/* (the data starts after the header, so the header counts: this keeps the read inside the mapped database) */
			if (entry[i].length >= sizeof(aux) &&
			    sizeof(*db) + slave->data_offset + entry[i].offset + sizeof(aux) <= CMD_DB_SIZE) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_PHYS                0x18220000UL
#define RSC_SIZE                0x10000UL
#define RSC_TCS(n, reg)         (0xd00 + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(RSC_PHYS, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (hakios_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see hakios_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as drivers/input/hakios.goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	bool done = false;

	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	ddr_voted = ddr_vote(vote);
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0, ddr_voted ? "" : ": FAILED");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void hakios_ddr_quiesce(void);

void
hakios_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, membench = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("cpufreq_max", &cpufreq_max_mhz, sizeof(cpufreq_max_mhz));
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[domain_of(topology->cpus[i].phys_id)]++;
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	gov_start();            /* darwin-lisa: the governor (cpugov=0: not) */
	/*
	 * darwin-lisa: a fast core runs out of work. Under this hypervisor a vCPU in WFI is not woken by its virtual timer
	 * (the only interrupt this port has), so the first idle moment is the end of the kernel: a few minutes into every
	 * cpufreq=1 boot, once the daemons settle. Slow, the core never idled. The idle loop must poll
	 * (kernel/patches/cpu-idle-no-wfi.patch makes wfi=0 the default; boot-arg wfi=0 does it for an older kernel).
	 */
	{
		extern unsigned int wfi;

		if (wfi != 0) {
			tell("cpufreq: WARNING: the idle loop uses WFI (wfi=%u): on this hypervisor the first idle moment hangs the "
			    "kernel; boot with wfi=0\n", wfi);
		}
	}
	/* membench has its own variable: when the boot-arg is absent, the cpufreq value (2 means on) must not start the benchmark */
	PE_parse_boot_argn("membench", &membench, sizeof(membench));
	if (membench == 1) {
		memory_bench();
	} else if (membench == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
