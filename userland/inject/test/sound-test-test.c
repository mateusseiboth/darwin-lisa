/* Host test of sound-test.c (libdlisa: boot-arg dlisa_sndtest=SECS[:SOUND],... in SpringBoard) */
#include <stdio.h>

#include "sound-test.c"

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
	struct soundTestStep steps[SOUND_TEST_STEPS];
	unsigned n;

	n = parseSoundTest("lisa=1 sound=4 adsp=1", steps);
	expect(n == 0, "absent: nothing");
	n = parseSoundTest("lisa=1 dlisa_sndtest=40 x=1", steps);
	expect(n == 1 && steps[0].seconds == 40 && strcmp(steps[0].sound, "ReceivedMessage") == 0, "one time, the default sound");
	n = parseSoundTest("dlisa_sndtest=40:alarm,55,70:Tock", steps);
	expect(n == 3 && steps[0].seconds == 40 && strcmp(steps[0].sound, "alarm") == 0 && steps[1].seconds == 55 &&
	    strcmp(steps[1].sound, "ReceivedMessage") == 0 && steps[2].seconds == 70 && strcmp(steps[2].sound, "Tock") == 0,
	    "times and sounds");
	n = parseSoundTest("dlisa_sndtest=40:../../etc/passwd", steps);
	expect(n == 1 && strcmp(steps[0].sound, "ReceivedMessage") == 0, "a name with a path in it is not taken");
	n = parseSoundTest("dlisa_sndtest=1,2,3,4,5,6,7,8,9,10,11,12", steps);
	expect(n == SOUND_TEST_STEPS, "at most SOUND_TEST_STEPS");
	n = parseSoundTest("xdlisa_sndtest=40", steps);
	expect(n == 0, "a whole word");
	expect(strcmp(soundTestPath("alarm", (char[96]){0}, 96), "/System/Library/Audio/UISounds/alarm.caf") == 0, "the file");
	printf("sound-test-test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
