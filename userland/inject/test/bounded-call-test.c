/* Host test of bounded-call.c (libdlisa: a blocking call given a deadline) */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "bounded-call.c"

static int failures;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static void
answerAtOnce(void *context)
{
	*(int *)context = 42;
}

static int lateRuns;

static void
answerLate(void *context)
{
	(void)context;
	usleep(300 * 1000);
	__atomic_add_fetch(&lateRuns, 1, __ATOMIC_SEQ_CST);
}

static double
secondsSince(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

int
main(void)
{
	struct timespec start;
	int answer = 0;

	expect(runBounded(answerAtOnce, &answer, 1000), "a quick call finishes");
	expect(answer == 42, "its result is there");

	clock_gettime(CLOCK_MONOTONIC, &start);
	expect(!runBounded(answerLate, NULL, 50), "a call past its deadline is given up");
	expect(secondsSince(&start) < 0.25, "the caller waits only the deadline");

	usleep(500 * 1000);
	expect(__atomic_load_n(&lateRuns, __ATOMIC_SEQ_CST) == 1, "the late call still ends on its own thread");

	expect(runBounded(answerLate, NULL, 2000), "a slow call within its deadline finishes");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
