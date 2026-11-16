/*
 * dlisa-audio-logic.h on the host: which devices the plug-in publishes (boot-args dlisa_audio=, dlisa_audio_devs=), the
 * volume control's scale and what goes to the kernel, and the device clock's zero time stamps.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../dlisa-audio-logic.h"

static int failures;

#define CHECK(what, cond) do { if (!(cond)) { printf("FAIL: %s\n", what); failures++; } } while (0)

static bool
near(double a, double b)
{
	return fabs(a - b) < 1e-4;
}

int
main(void)
{
	struct dlisa_audio_choice c;

	dlisa_audio_choose(&c, "dlisa_unlock=1 lisa=1 sound=4 adsp=1");
	CHECK("default: the four of HakiOS", c.on && c.roles == (DLISA_ROLE_SPEAKER | DLISA_ROLE_HAWKING | DLISA_ROLE_ACTUATOR |
	    DLISA_ROLE_CODEC));
	CHECK("default UIDs", strcmp(c.uid[DLISA_SPEAKER], "Speaker") == 0 && strcmp(c.uid[DLISA_HAWKING], "Hawking") == 0 &&
	    strcmp(c.uid[DLISA_RECEIVER], "Receiver") == 0);

	dlisa_audio_choose(&c, "lisa=1 dlisa_audio=0 sound=4");
	CHECK("dlisa_audio=0: nothing published", !c.on);

	dlisa_audio_choose(&c, "lisa=1 dlisa_audio_devs=speaker,hawking,receiver sound=4");
	CHECK("a list", c.on && c.roles == (DLISA_ROLE_SPEAKER | DLISA_ROLE_HAWKING | DLISA_ROLE_RECEIVER));

	dlisa_audio_choose(&c, "dlisa_audio_devs=speaker:Speaker1,codec:Codec,bogus x=1");
	CHECK("a renamed device", c.roles == (DLISA_ROLE_SPEAKER | DLISA_ROLE_CODEC) &&
	    strcmp(c.uid[DLISA_SPEAKER], "Speaker1") == 0 && strcmp(c.uid[DLISA_CODEC], "Codec") == 0);

	dlisa_audio_choose(&c, "xdlisa_audio_devs=codec dlisa_audio=1");
	CHECK("a key is a whole word", c.roles != DLISA_ROLE_CODEC && c.on);

	dlisa_audio_choose(&c, "dlisa_audio_devs=speaker:ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ");
	CHECK("a long UID is cut, not overrun", strlen(c.uid[DLISA_SPEAKER]) < sizeof(c.uid[DLISA_SPEAKER]));

	CHECK("scalar 1 is 0 dB", near(dlisa_audio_scalar_to_db(1.0f), 0));
	CHECK("scalar 0 is the bottom", near(dlisa_audio_scalar_to_db(0.0f), -DLISA_AUDIO_VOLUME_RANGE_DB));
	CHECK("round trip", near(dlisa_audio_db_to_scalar(dlisa_audio_scalar_to_db(0.25f)), 0.25));
	CHECK("clamped", near(dlisa_audio_db_to_scalar(-100), 0) && near(dlisa_audio_db_to_scalar(5), 1));
	{
		struct dlisa_audio_kernel_volume k = dlisa_audio_kernel_volume(0.5f);

		CHECK("half: 24 dB under, in eighths", k.below == 192 && !k.muted);
		k = dlisa_audio_kernel_volume(0.0f);
		CHECK("0 is muted", k.muted);
	}

	{
		struct dlisa_audio_clock clk = { .anchor = 1000, .periods = 0 };
		double sample;
		uint64_t host;

		/* 48 kHz on a 24 MHz timebase: 500 ticks a frame, a period of 16384 frames = 8192000 ticks */
		dlisa_audio_zero_time(&clk, 1000 + 8192000 * 2 + 5, 500.0, 16384, &sample, &host);
		CHECK("two periods in", clk.periods == 2 && near(sample, 32768) && host == 1000 + 8192000 * 2);
		dlisa_audio_zero_time(&clk, 1000 + 8192000 * 2 + 6, 500.0, 16384, &sample, &host);
		CHECK("monotonic", clk.periods == 2);
	}

	printf("%s\n", failures ? "dlisa-audio logic: FAILURES" : "dlisa-audio logic: all passed");
	return failures != 0;
}
