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
 *   touch_seq=G@S,G@S,...   named gestures, each S seconds after bsd_init (a list, in order; independent of touch_synth):
 *                           nc   swipe down from the top-left edge (Notification Center)
 *                           cc   swipe down from the top-right edge (Control Center)
 *                           sw   swipe up from the home bar and hold before lifting (the app switcher)
 *                           up   swipe up from the home bar (home / dismiss)
 *                           tap  a tap at the middle (dismiss)
 *                           e.g. touch_seq=nc@90,up@120,cc@150,up@180,sw@210
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <string.h>

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

/* touch_seq: a straight swipe from (x0, y0) to (x1, y1) in `steps` moves 16 ms apart, held `hold_ms` before lifting */
static void
seq_swipe(const char *name, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t steps, uint32_t hold_ms)
{
	hakios_touch_post(HAKIOS_TOUCH_DOWN, SYNTH_FINGER, x0, y0);
	for (uint32_t i = 1; i <= steps; i++) {
		synth_sleep_ms(16);
		hakios_touch_post(HAKIOS_TOUCH_MOVE, SYNTH_FINGER, x0 + ((int64_t)x1 - x0) * i / steps, y0 + ((int64_t)y1 - y0) * i / steps);
	}
	if (hold_ms) {
		/* a finger that stays put still reports: the recogniser needs the pause to see a hold */
		for (uint32_t t = 0; t < hold_ms; t += 50) {
			synth_sleep_ms(50);
			hakios_touch_post(HAKIOS_TOUCH_MOVE, SYNTH_FINGER, x1, y1);
		}
	}
	synth_sleep_ms(16);
	hakios_touch_post(HAKIOS_TOUCH_UP, SYNTH_FINGER, x1, y1);
	printf("touch_synth: seq %s posted (%u, %u) -> (%u, %u), %u moves, held %u ms\n", name, x0, y0, x1, y1, steps, hold_ms);
}

static uint32_t
seq_number(const char *s)
{
	uint32_t n = 0;

	while (*s >= '0' && *s <= '9') {
		n = n * 10 + (uint32_t)(*s++ - '0');
	}
	return n > 65535 ? 65535 : n;
}

static void
seq_gesture(const char *g)
{
	/* 0..65535: the top edge strip is 0.3 %, the home bar 99 %, the panel's sides at 15 % and 88 % */
	if (strncmp(g, "nc", 2) == 0) {
		seq_swipe("nc", 9830, 200, 9830, 45875, 25, 0);
	} else if (strncmp(g, "cc", 2) == 0) {
		seq_swipe("cc", 57671, 200, 57671, 45875, 25, 0);
	} else if (strncmp(g, "sw", 2) == 0) {
		seq_swipe("sw", 32768, 64880, 32768, 32768, 25, 1500);
	} else if (strncmp(g, "up", 2) == 0) {
		seq_swipe("up", 32768, 64880, 32768, 22937, 20, 0);
	} else if (strncmp(g, "tap", 3) == 0) {
		/* tap: the middle; tap:X:Y (0..65535): an icon, a row (the home screen's Settings icon on the classic layout
		 * is about tap:56800:5600) */
		uint32_t x = 32768, y = 32768;

		if (g[3] == ':') {
			x = seq_number(g + 4);
			y = seq_number(strchr(g + 4, ':') != NULL ? strchr(g + 4, ':') + 1 : "32768");
		}
		hakios_touch_post(HAKIOS_TOUCH_DOWN, SYNTH_FINGER, x, y);
		synth_sleep_ms(80);
		hakios_touch_post(HAKIOS_TOUCH_UP, SYNTH_FINGER, x, y);
		printf("touch_synth: seq tap posted at (%u, %u)\n", x, y);
	} else {
		printf("touch_synth: seq: unknown gesture \"%s\"\n", g);
	}
}

static char seq_spec[160];

static void
seq_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t start, now;
	char *p = seq_spec;

	clock_get_uptime(&start);
	while (*p) {
		char *at, *next = p;
		uint32_t secs = 0;

		while (*next && *next != ',') {
			next++;
		}
		if (*next) {
			*next++ = '\0';
		}
		at = p;
		while (*at && *at != '@') {
			at++;
		}
		if (*at) {
			*at++ = '\0';
			while (*at >= '0' && *at <= '9') {
				secs = secs * 10 + (uint32_t)(*at++ - '0');
			}
		}
		for (;;) {
			uint64_t ns;

			clock_get_uptime(&now);
			absolutetime_to_nanoseconds(now - start, &ns);
			if (ns / NSEC_PER_SEC >= secs) {
				break;
			}
			synth_sleep_ms(200);
		}
		seq_gesture(p);
		p = next;
	}
	printf("touch_synth: seq done\n");
	thread_terminate(current_thread());
}

static void
seq_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("touch_seq", seq_spec, sizeof(seq_spec)) || seq_spec[0] == '\0') {
		return;
	}
	printf("touch_synth: seq \"%s\"\n", seq_spec);
	if (kernel_thread_start(seq_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void dlisa_touch_synth_init(void);

void
dlisa_touch_synth_init(void)
{
	thread_t thread;

	seq_init();
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
