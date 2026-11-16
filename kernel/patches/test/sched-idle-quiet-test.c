/*
 * darwin-lisa: host test of sched-idle-quiet.patch's decisions (which idle cores sleep in WFI, and that a core that
 * holds a head of the band polls instead). run.sh pulls the patch's helper block out of sched-idle-quiet.patch into
 * sched-idle-quiet-block.h and builds this file against the stand-ins below.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_CPUS 8
#define TUNABLE(type, name, arg, value) static type name = value

#include "sched-idle-quiet-block.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void
reset(void)
{
	lisa_idle_wfi_mask = 0x70;
	for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
		lisa_idle_mark(cpu, false);
	}
}

/* the default: the A55s (cpu4-6) sleep, the A78s (cpu0-3) poll; wfi=0 */
static void
test_default_mask_sleeps_only_the_small_cores(void)
{
	reset();
	for (int cpu = 0; cpu < 4; cpu++) {
		CHECK(!dlisa_idle_wfi(cpu, 0));
	}
	for (int cpu = 4; cpu < 7; cpu++) {
		CHECK(dlisa_idle_wfi(cpu, 0));
	}
}

/* wfi=1 (the old boot-arg) puts every core to sleep, as before */
static void
test_wfi_boot_arg_sleeps_every_core(void)
{
	reset();
	for (int cpu = 0; cpu < 7; cpu++) {
		CHECK(dlisa_idle_wfi(cpu, 1));
	}
}

/* idle_wfi_mask=0: every core polls, the kernel before this patch */
static void
test_empty_mask_polls_everywhere(void)
{
	reset();
	lisa_idle_wfi_mask = 0;
	for (int cpu = 0; cpu < 7; cpu++) {
		CHECK(!dlisa_idle_wfi(cpu, 0));
	}
}

/* a small core that leaves a head of the band to the A78s (lisa_hold) keeps polling: its wait stays bounded */
static void
test_a_holding_core_polls_until_it_lets_go(void)
{
	reset();
	lisa_idle_mark(5, true);
	CHECK(!dlisa_idle_wfi(5, 0));
	CHECK(!dlisa_idle_wfi(5, 1));
	CHECK(dlisa_idle_wfi(4, 0));            /* the others are not affected */
	lisa_idle_mark(5, false);
	CHECK(dlisa_idle_wfi(5, 0));
}

/* each core's flag on a cache line of its own: the cores write it on every turn of their idle loop */
static void
test_each_core_owns_a_cache_line(void)
{
	CHECK((uintptr_t)&lisa_idle_cpu[1] - (uintptr_t)&lisa_idle_cpu[0] == 64);
	CHECK((uintptr_t)&lisa_idle_cpu[0] % 64 == 0);
}

int
main(void)
{
	test_default_mask_sleeps_only_the_small_cores();
	test_wfi_boot_arg_sleeps_every_core();
	test_empty_mask_polls_everywhere();
	test_a_holding_core_polls_until_it_lets_go();
	test_each_core_owns_a_cache_line();
	printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
