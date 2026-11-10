/*
 * darwin-lisa diagnostic: where the threads of a user process are (boot-arg psample=NAME, psample_secs=N).
 *
 * Every N seconds (default 45, from the first call on), for each process whose name begins with NAME: per thread its
 * state, what last blocked it (block_hint), the system call it is in (x16) and the user frames (pc, lr, then the frame
 * pointer chain, eight deep). The user addresses are the shared cache's slid ones; "slide" is printed with them. Once
 * logd has the log, this is the only way to see what a daemon waits for. Lines: "psample ...".
 */
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <arm/thread.h>
#include <mach/arm/thread_status.h>
#include <vm/vm_kern_xnu.h>
#include <vm/vm_map_xnu.h>
#include <vm/vm_shared_region_xnu.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <string.h>

extern void *get_bsdtask_info(task_t);
extern char *proc_name_address(void *p);
void dlisa_sample_start(void);

#define SAMPLE_THREADS  48
#define SAMPLE_FRAMES   40

static char gSampleName[128];
static uint32_t gSampleSeconds = 45;

struct sample {
	uint64_t pc, lr, fp, x16, x0, x1;
	int state, hint;
};

static void
sample_task(task_t task, int pid, const char *name)
{
	static struct sample s[SAMPLE_THREADS];
	unsigned n = 0;
	thread_t thread;
	vm_map_t map;
	uint32_t slide = 0;

	task_lock(task);
	queue_iterate(&task->threads, thread, thread_t, task_threads) {
		arm_saved_state_t *ss = get_user_regs(thread);

		if (n >= SAMPLE_THREADS || ss == NULL) {
			continue;
		}
		s[n].pc = get_saved_state_pc(ss);
		s[n].lr = get_saved_state_lr(ss);
		s[n].fp = get_saved_state_fp(ss);
		s[n].x16 = get_saved_state_reg(ss, 16);
		s[n].x0 = get_saved_state_reg(ss, 0);
		s[n].x1 = get_saved_state_reg(ss, 1);
		s[n].state = thread->state;
		s[n].hint = thread->block_hint;
		n++;
	}
	map = task->map;
	vm_map_reference(map);
	if (task->shared_region != NULL) {
		slide = task->shared_region->sr_slide;
	}
	task_unlock(task);

	printf("psample %s[%d]: %u threads, slide 0x%x\n", name, pid, n, slide);
	for (unsigned i = 0; i < n; i++) {
		char line[1024];
		size_t used;
		uint64_t fp = s[i].fp;

		used = (size_t)snprintf(line, sizeof(line), "psample %d t%u st%x hint%d x16=%lld x0=%llx x1=%llx pc %llx lr %llx", pid, i,
		    s[i].state, s[i].hint, (long long)s[i].x16, s[i].x0, s[i].x1, s[i].pc, s[i].lr);
		for (int f = 0; f < SAMPLE_FRAMES && fp != 0 && (fp & 7) == 0 && used + 20 < sizeof(line); f++) {
			uint64_t frame[2];

			if (copyinmap(map, (vm_map_offset_t)fp, frame, sizeof(frame)) != KERN_SUCCESS) {
				break;
			}
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %llx", frame[1]);
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
		printf("%s\n", line);
	}
	vm_map_deallocate(map);
}

/* psample=A,B,C: a process is sampled when its name begins with one of them */
static bool
sample_wanted(const char *name)
{
	const char *at = gSampleName;

	while (*at != 0) {
		const char *end = strchr(at, ',');
		size_t len = end ? (size_t)(end - at) : strlen(at);

		if (len != 0 && strncmp(name, at, len) == 0) {
			return true;
		}
		at += len + (end ? 1 : 0);
	}
	return false;
}

static void
sample_all(void)
{
	task_t found[12];
	int pids[12];
	char names[12][32];
	unsigned n = 0;
	task_t task;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *p = get_bsdtask_info(task);
		const char *name;

		if (p == NULL || n >= 12) {
			continue;
		}
		name = proc_name_address(p);
		if (!sample_wanted(name)) {
			continue;
		}
		task_reference(task);
		found[n] = task;
		pids[n] = task_pid(task);
		strlcpy(names[n], name, sizeof(names[n]));
		n++;
	}
	lck_mtx_unlock(&tasks_threads_lock);
	for (unsigned i = 0; i < n; i++) {
		sample_task(found[i], pids[i], names[i]);
		task_deallocate(found[i]);
	}
}

static void
sample_main(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		delay_for_interval((uint64_t)gSampleSeconds * 1000, NSEC_PER_MSEC);
		sample_all();
	}
}

void
dlisa_sample_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_arg_str("psample", gSampleName, sizeof(gSampleName)) || gSampleName[0] == 0) {
		return;
	}
	PE_parse_boot_argn("psample_secs", &gSampleSeconds, sizeof(gSampleSeconds));
	if (gSampleSeconds == 0) {
		gSampleSeconds = 45;
	}
	if (kernel_thread_start(sample_main, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		printf("psample: %s every %u s\n", gSampleName, gSampleSeconds);
	}
}
