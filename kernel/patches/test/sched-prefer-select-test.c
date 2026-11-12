/*
 * darwin-lisa: host test of sched-prefer-select.patch's decisions (lisa_hold, lisa_should_move, lisa_took).
 * run.sh pulls the patch's helper block out of sched-prefer-select.patch into sched-prefer-select-block.h and builds
 * this file against the small stand-ins below; the kernel's own types are not involved.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CPUS 8
#define NOPRI (-1)
#define BASEPRI_RTQUEUES 97
#define NSEC_PER_USEC 1000ull
#define TH_IDLE 0x80
#define TH_MODE_REALTIME 1
#define TUNABLE(type, name, arg, value) static type name = value
#define relaxed 0
#define os_atomic_inc(p, order) ((void)(order), ++*(p))
#define os_atomic_load(p, order) ((void)(order), *(p))

enum { PROCESSOR_OFF_LINE, PROCESSOR_IDLE, PROCESSOR_DISPATCHING, PROCESSOR_RUNNING, PROCESSOR_STATE_LEN };
typedef enum { SCHED_IPI_NONE, SCHED_IPI_IMMEDIATE } sched_ipi_type_t;
enum { SCHED_IPI_EVENT_REBALANCE };

struct processor_set {
	uint64_t perfcontrol_cpu_preferred_bitmask, cpu_state_map[PROCESSOR_STATE_LEN], cpu_available_map,
	    recommended_bitmask, realtime_map, pending_AST_URGENT_cpu_mask, pending_AST_PREEMPT_cpu_mask;
	int rt_count, root_pri;
};
typedef struct processor_set *processor_set_t;
struct processor {
	int cpu_id, state, current_pri, bound_count;
	bool current_is_bound;
	uint64_t last_dispatch;
	processor_set_t processor_set;
};
typedef struct processor *processor_t;
struct thread {
	void *bound_processor;
	int state, sched_pri, sched_mode;
};
typedef struct thread *thread_t;
#define PROCESSOR_NULL ((void *)0)

static uint64_t now_abs;
static int ipis;
static struct processor cpus[MAX_CPUS];
static processor_t processor_array[MAX_CPUS] = { &cpus[0], &cpus[1], &cpus[2], &cpus[3], &cpus[4], &cpus[5], &cpus[6], &cpus[7] };
static int lisa_sched_prefer_pri = 37;

static uint64_t mach_absolute_time(void) { return now_abs; }
static void clock_interval_to_absolutetime_interval(uint32_t v, uint64_t scale, uint64_t *out) { *out = v * scale; }
static uint64_t pset_available_cpumap(processor_set_t p) { return p->cpu_available_map & p->recommended_bitmask; }
static int rt_runq_count(processor_set_t p) { return p->rt_count; }
static int sched_clutch_lisa_root_priority(processor_set_t p) { return p->root_pri; }
static bool bit_test(uint64_t map, int bit) { return (map >> bit) & 1; }
static int bit_count(uint64_t map) { return __builtin_popcountll(map); }
static int lsb_first(uint64_t map) { return map ? __builtin_ctzll(map) : -1; }
static int lsb_next(uint64_t map, int prev) { uint64_t m = prev >= 63 ? 0 : map & ~((2ull << prev) - 1); return lsb_first(m); }
static void pset_lock(processor_set_t p) { (void)p; }
static void pset_unlock(processor_set_t p) { (void)p; }
static sched_ipi_type_t sched_ipi_action(processor_t dst, void *t, int e) { (void)dst; (void)t; (void)e; return SCHED_IPI_IMMEDIATE; }
static void sched_ipi_perform(processor_t dst, sched_ipi_type_t i) { (void)dst; (void)i; ipis++; }
#define SCHED(f) sched_##f
static int sched_processor_bound_count(processor_t p) { return p->bound_count; }

#include "sched-prefer-select-block.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static struct processor_set pset;

/* the phone's layout: cpu0-3 A78 (sched_prefer=0xf), cpu4-6 A55, everything online and idle */
static void
reset(void)
{
	memset(&pset, 0, sizeof(pset));
	memset(cpus, 0, sizeof(cpus));
	memset(lisa_hold_since, 0, sizeof(lisa_hold_since));
	memset(lisa_hold_seen, 0, sizeof(lisa_hold_seen));
	memset(lisa_pull_looked, 0, sizeof(lisa_pull_looked));
	dlisa_sp_held = dlisa_sp_late = dlisa_sp_hand = dlisa_sp_move = dlisa_sp_pull = 0;
	pset.perfcontrol_cpu_preferred_bitmask = 0xf;
	pset.cpu_available_map = pset.recommended_bitmask = 0x7f;
	pset.cpu_state_map[PROCESSOR_IDLE] = 0x7f;
	pset.root_pri = NOPRI;
	for (int i = 0; i < 7; i++) {
		cpus[i].cpu_id = i;
		cpus[i].state = PROCESSOR_IDLE;
		cpus[i].processor_set = &pset;
	}
	lisa_sched_prefer_wait_us = 2000;
	lisa_sched_prefer_pull_us = 200;
	lisa_wait_abs = lisa_pull_abs = 0;
	now_abs = 1000000;
	ipis = 0;
}

static void
test_fast_core_never_holds(void)
{
	reset();
	pset.root_pri = 47;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[0]));
	CHECK(!lisa_hold_timeshare(&pset, &cpus[3]));
}

static void
test_slow_core_holds_band_then_takes_it_late(void)
{
	reset();
	pset.root_pri = 47;
	CHECK(lisa_hold_timeshare(&pset, &cpus[5]));
	now_abs += 1999 * NSEC_PER_USEC;
	CHECK(lisa_hold_timeshare(&pset, &cpus[5]));
	now_abs += 1 * NSEC_PER_USEC;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[5]));
	CHECK(!lisa_hold_timeshare(&pset, &cpus[5]));     /* the idle loop broke, thread_select looks again: still released */
	CHECK(dlisa_sp_held == 1 && dlisa_sp_late == 1);
}

static void
test_below_band_is_not_held(void)
{
	reset();
	pset.root_pri = 31;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[4]));
	pset.root_pri = NOPRI;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[4]));
	CHECK(dlisa_sp_held == 0);
}

static void
test_a_gap_starts_a_new_wait(void)
{
	reset();
	pset.root_pri = 47;
	CHECK(lisa_hold_timeshare(&pset, &cpus[6]));
	now_abs += 5000 * NSEC_PER_USEC;                  /* the core ran something else for 5 ms */
	CHECK(lisa_hold_timeshare(&pset, &cpus[6]));
	CHECK(dlisa_sp_held == 2 && dlisa_sp_late == 0);
}

static void
test_taking_a_thread_ends_the_wait(void)
{
	reset();
	pset.root_pri = 47;
	CHECK(lisa_hold_timeshare(&pset, &cpus[4]));
	now_abs += 1500 * NSEC_PER_USEC;                  /* the idle loop keeps looking */
	CHECK(lisa_hold_timeshare(&pset, &cpus[4]));
	now_abs += 1000 * NSEC_PER_USEC;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[4]));
	lisa_took(&cpus[4]);
	now_abs += 10 * NSEC_PER_USEC;
	CHECK(lisa_hold_timeshare(&pset, &cpus[4]));     /* the next thread of the band waits again */
}

static void
test_bound_threads_are_never_held(void)
{
	reset();
	pset.root_pri = 47;
	cpus[5].bound_count = 1;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[5]));
}

static void
test_off_without_mask_or_wait_or_fast_cores(void)
{
	reset();
	pset.root_pri = 47;
	lisa_sched_prefer_wait_us = 0;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[5]));
	reset();
	pset.root_pri = 47;
	pset.perfcontrol_cpu_preferred_bitmask = 0;
	CHECK(!lisa_hold_timeshare(&pset, &cpus[5]));
	reset();
	pset.root_pri = 47;
	pset.cpu_available_map = 0x70;                    /* cpus=... with only A55s online */
	CHECK(!lisa_hold_timeshare(&pset, &cpus[5]));
}

static void
test_realtime_waits_only_for_an_a78_free_of_rt(void)
{
	reset();
	pset.rt_count = 1;
	CHECK(lisa_hold_realtime(&pset, &cpus[4]));
	reset();
	pset.rt_count = 1;
	pset.realtime_map = 0xf;                          /* every A78 runs RT: the A55 takes it */
	CHECK(!lisa_hold_realtime(&pset, &cpus[4]));
	reset();
	pset.rt_count = 2;
	pset.realtime_map = 0x7;                          /* one A78 free, two waiting */
	CHECK(!lisa_hold_realtime(&pset, &cpus[4]));
	reset();
	CHECK(!lisa_hold_realtime(&pset, &cpus[4]));      /* empty RT queue */
}

static void
test_rt_and_timeshare_waits_are_separate(void)
{
	reset();
	pset.rt_count = 1;
	pset.root_pri = 20;
	CHECK(lisa_hold_realtime(&pset, &cpus[4]));
	CHECK(!lisa_hold_timeshare(&pset, &cpus[4]));     /* a low head resets the timeshare wait only */
	now_abs += 1000 * NSEC_PER_USEC;
	CHECK(lisa_hold_realtime(&pset, &cpus[4]));
	now_abs += 1000 * NSEC_PER_USEC;
	CHECK(!lisa_hold_realtime(&pset, &cpus[4]));
}

static void
test_should_move(void)
{
	struct thread ui = { .sched_pri = 47 }, low = { .sched_pri = 20 }, bound = { .sched_pri = 47, .bound_processor = &cpus[5] };
	struct thread idle = { .sched_pri = 47, .state = TH_IDLE };

	reset();
	CHECK(lisa_should_move(&pset, &cpus[5], &ui));
	CHECK(!lisa_should_move(&pset, &cpus[1], &ui));   /* already on an A78 */
	CHECK(!lisa_should_move(&pset, &cpus[5], &low));
	CHECK(!lisa_should_move(&pset, &cpus[5], &bound));
	CHECK(!lisa_should_move(&pset, &cpus[5], &idle));
	pset.cpu_state_map[PROCESSOR_IDLE] = 0x70;        /* every A78 busy */
	CHECK(!lisa_should_move(&pset, &cpus[5], &ui));
	CHECK(!lisa_handoff_away(&cpus[5], &ui));
	pset.cpu_state_map[PROCESSOR_IDLE] = 0x7f;
	CHECK(lisa_handoff_away(&cpus[5], &ui) && dlisa_sp_hand == 1);
}

static void
test_pull(void)
{
	reset();
	pset.cpu_state_map[PROCESSOR_IDLE] = 0x2f;        /* cpu0-3 idle, cpu5 idle, cpu4 and 6 run */
	pset.cpu_state_map[PROCESSOR_RUNNING] = 0x50;
	cpus[4].state = cpus[6].state = PROCESSOR_RUNNING;
	cpus[4].current_pri = 47;
	cpus[4].last_dispatch = now_abs - 100 * NSEC_PER_USEC;
	cpus[6].current_pri = 20;
	cpus[6].last_dispatch = now_abs - 900 * NSEC_PER_USEC;
	lisa_pull(&pset, &cpus[4]);                       /* not from a slow core */
	lisa_pull(&pset, &cpus[0]);                       /* cpu4 has run its UI thread only 100 us, cpu6 runs below the band */
	CHECK(ipis == 0);
	now_abs += 150 * NSEC_PER_USEC;
	lisa_pull(&pset, &cpus[0]);
	CHECK(ipis == 1 && dlisa_sp_pull == 1);
	lisa_pull(&pset, &cpus[0]);                       /* looked a moment ago */
	CHECK(ipis == 1);
	lisa_sched_prefer_pull_us = 0;
	now_abs += 1000 * NSEC_PER_USEC;
	lisa_pull(&pset, &cpus[1]);
	CHECK(ipis == 1);
}

static void
test_stats_line(void)
{
	char line[128];

	reset();
	dlisa_sched_prefer_line(line, sizeof(line));
	dlisa_sp_held = 3;
	dlisa_sp_pull = 1;
	dlisa_sched_prefer_line(line, sizeof(line));
	CHECK(strcmp(line, "tl: sp held 3 late 0 hand 0 move 0 pull 1") == 0);
	dlisa_sched_prefer_line(line, sizeof(line));
	CHECK(line[0] == 0);
}

int
main(void)
{
	test_fast_core_never_holds();
	test_slow_core_holds_band_then_takes_it_late();
	test_below_band_is_not_held();
	test_a_gap_starts_a_new_wait();
	test_taking_a_thread_ends_the_wait();
	test_bound_threads_are_never_held();
	test_off_without_mask_or_wait_or_fast_cores();
	test_realtime_waits_only_for_an_a78_free_of_rt();
	test_rt_and_timeshare_waits_are_separate();
	test_should_move();
	test_pull();
	test_stats_line();
	printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
