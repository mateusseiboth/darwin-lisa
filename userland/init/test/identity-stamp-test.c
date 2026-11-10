/* Host test of identity-stamp.c (init26: MobileGestalt's cache goes when the phone's identity changes) */
#include <stdio.h>
#include <string.h>

#include "identity-stamp.c"

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
	char stamp[64];

	buildIdentityStamp(stamp, sizeof(stamp), "iPhone14,4", "D16AP");
	expect(strcmp(stamp, "iPhone14,4 D16AP\n") == 0, "stamp: machine, model, newline");
	expect(!isIdentityChanged("iPhone14,4 D16AP\n", 17, stamp), "same stamp: unchanged");
	expect(isIdentityChanged("iPhone10,3 D22AP\n", 17, stamp), "other phone: changed");
	expect(isIdentityChanged("", 0, stamp), "no stamp yet: changed");
	expect(isIdentityChanged("iPhone14,4 D16AP\nX", 18, stamp), "longer stamp: changed");
	buildIdentityStamp(stamp, 8, "iPhone14,4", "D16AP");
	expect(strlen(stamp) < 8, "small buffer: cut, still a string");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
