/* Host test of launch-probe.c (libdlisa, dlisa_launchprobe=: the main thread sampled while a program starts) */
#include <stdio.h>

#include "launch-probe.c"

static int failures;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

int
main(void)
{
	expect(!isLaunchProbeDue(0, 0), "nothing at the start");
	expect(!isLaunchProbeDue(1499, 0), "nothing before the first time");
	expect(isLaunchProbeDue(1500, 0), "the first sample at 1.5 s");
	expect(!isLaunchProbeDue(1600, 1), "the second waits for its own time");
	expect(isLaunchProbeDue(4000, 1), "the second at 4 s");
	expect(isLaunchProbeDue(9000, 1), "a late look takes the one it missed");
	expect(isLaunchProbeDue(9500, 3), "the last at 9.5 s, inside chronod's 10 s watchdog");
	expect(!isLaunchProbeDue(60000, LAUNCH_PROBE_SAMPLES), "no more after the last");
	expect(!isLaunchProbeDue(60000, -1), "a bad count takes nothing");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
