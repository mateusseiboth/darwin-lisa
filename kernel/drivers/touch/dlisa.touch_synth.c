/*
 * darwin-lisa: a synthetic swipe up, posted into /dev/hakios-touch as if the touch screen had sent it.
 *
 * A test hook for the touch path without a finger on the glass: the samples take the same road as the Goodix
 * driver's (hakios_touch_post -> /dev/hakios-touch -> libdlisa's HID service inside backboardd -> SpringBoard).
 * On the cover sheet a swipe up from the home bar opens the phone, which a screenshot shows.
 *
 * Boot-args (all off by default):
 *   touch_synth=SECS        the first swipe SECS seconds after this is started (bsd_init)
 *   touch_synth_every=SECS  then one more every SECS seconds (0: only the first)
 *   touch_synth_count=N     at most N swipes (default 1, or 10 with touch_synth_every)
 *   touch_synth_steps=N     moves in a swipe (default 20, one every 16 ms)
 *   touch_synth_tap=1       a tap before each swipe, at the middle of the screen or at touch_synth_x/_y
 *   touch_synth_tap=2       a tap only, no swipe (touch_synth_x=58060 touch_synth_y=62030: the cover sheet's (i))
 *   touch_synth_x=N, touch_synth_y=N   where the tap lands, 0..65535 (default the middle)
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);

#define SYNTH_FINGER    9      /* a finger number the touch screen itself does not use for one finger */

static uint32_t synth_delay_s, synth_every_s, synth_count, synth_steps = 20, synth_tap, synth_x = 32768, synth_y = 32768;

static void
synth_sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* 0..65535 on both axes, 0 = left/top */
static void
synth_swipe_up(unsigned which)
{
	const uint32_t x = 32768, from_y = 64880 /* 99% down: inside the home bar strip */, to_y = 22937 /* 35% */;

	if (synth_tap) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, SYNTH_FINGER, synth_x, synth_y);
		synth_sleep_ms(80);
		hakios_touch_post(HAKIOS_TOUCH_UP, SYNTH_FINGER, synth_x, synth_y);
		printf("touch_synth: tap #%u posted at (%u, %u)\n", which, synth_x, synth_y);
		if (synth_tap == 2) {
			return;
		}
		synth_sleep_ms(400);
	}
	hakios_touch_post(HAKIOS_TOUCH_DOWN, SYNTH_FINGER, x, from_y);
	for (uint32_t i = 1; i <= synth_steps; i++) {
		synth_sleep_ms(16);
		hakios_touch_post(HAKIOS_TOUCH_MOVE, SYNTH_FINGER, x, from_y - (from_y - to_y) * i / synth_steps);
	}
	synth_sleep_ms(16);
	hakios_touch_post(HAKIOS_TOUCH_UP, SYNTH_FINGER, x, to_y);
	printf("touch_synth: swipe up #%u posted (%u, %u) -> (%u, %u), %u moves\n", which, x, from_y, x, to_y, synth_steps);
}

static void
synth_thread(__unused void *parameter, __unused wait_result_t result)
{
	synth_sleep_ms(synth_delay_s * 1000);
	for (unsigned n = 1; n <= synth_count; n++) {
		synth_swipe_up(n);
		if (synth_every_s == 0) {
			break;
		}
		synth_sleep_ms(synth_every_s * 1000);
	}
	thread_terminate(current_thread());
}

void dlisa_touch_synth_init(void);

void
dlisa_touch_synth_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("touch_synth", &synth_delay_s, sizeof(synth_delay_s)) || synth_delay_s == 0) {
		return;
	}
	PE_parse_boot_argn("touch_synth_every", &synth_every_s, sizeof(synth_every_s));
	synth_count = synth_every_s ? 10 : 1;
	PE_parse_boot_argn("touch_synth_count", &synth_count, sizeof(synth_count));
	PE_parse_boot_argn("touch_synth_steps", &synth_steps, sizeof(synth_steps));
	PE_parse_boot_argn("touch_synth_tap", &synth_tap, sizeof(synth_tap));
	PE_parse_boot_argn("touch_synth_x", &synth_x, sizeof(synth_x));
	PE_parse_boot_argn("touch_synth_y", &synth_y, sizeof(synth_y));
	if (synth_steps == 0 || synth_steps > 200) {
		synth_steps = 20;
	}
	printf("touch_synth: %u swipe(s) up, the first in %u s, then every %u s\n", synth_count, synth_delay_s, synth_every_s);
	if (kernel_thread_start(synth_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
