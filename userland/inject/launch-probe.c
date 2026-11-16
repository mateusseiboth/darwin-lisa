/*
 * libdlisa, boot-arg dlisa_launchprobe=NAME,...: where the main thread of a program is while it starts.
 *
 * chronod's widget and control extensions (CalculatorWidget, LauncherControlExtension, ...) are killed by
 * RunningBoard 10 s after they start ("ChronodWidgetExtensionWatchdog") without a word: they never told chronod they
 * were ready. dlisa_uiprobe's stuck-thread samples wait for the main run loop to have waited once, which such a
 * program may never do. This one samples the main thread at fixed times after the library's constructor, whatever it
 * is doing: blocked in a call (its frames name it), or waiting in the run loop (then the other side never asked).
 *
 * Pure: dlisa-inject.c includes it, test/launch-probe-test.c too.
 */
#include <stdbool.h>
#include <stdint.h>

#define LAUNCH_PROBE_SAMPLES 4

/* milliseconds after the constructor; the last one inside chronod's 10 s */
static const uint64_t launchProbeTimesMs[LAUNCH_PROBE_SAMPLES] = { 1500, 4000, 7000, 9500 };

/* the sample number TAKEN (0 first) is due ELAPSED ms after the constructor */
static bool
isLaunchProbeDue(uint64_t elapsedMs, int taken)
{
	return taken >= 0 && taken < LAUNCH_PROBE_SAMPLES && elapsedMs >= launchProbeTimesMs[taken];
}
