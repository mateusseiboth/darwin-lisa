/* Host test of torch-level.c (libdlisa: AVFlashlight's level 0..1 as the kernel torch's 0..1000) */
#include <stdio.h>

#include "torch-level.c"

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
	expect(torchPermilleForLevel(0.0f) == 0, "0 is off");
	expect(torchPermilleForLevel(1.0f) == 1000, "1 is the most");
	expect(torchPermilleForLevel(0.5f) == 500, "a half is 500");
	expect(torchPermilleForLevel(1.7f) == 1000, "above 1 is clamped");
	expect(torchPermilleForLevel(-0.2f) == 0, "below 0 is off");
	expect(torchPermilleForLevel(0.0004f) == 1, "the least light that is not off stays on");
	expect(torchPermilleForLevel(0.0f / 0.0f) == 0, "NaN is off");
	expect(isTorchServing("level 3 lit 0 wanted 0 times 0 cut 0 broken 0"), "torch=3 serves Darwin");
	expect(!isTorchServing("level 2 lit 0 wanted 0 times 0 cut 0 broken 0"), "torch=2 does not");
	expect(!isTorchServing("level 3 lit 0 wanted 0 times 0 cut 0 broken 1"), "a broken driver does not");
	expect(!isTorchServing(NULL), "no driver does not");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
