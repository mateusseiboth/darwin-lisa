/* Host test of purgeable.c (libdlisa in PosterBoard: marking a file purgeable on a volume that is not APFS) */
#include <stdio.h>

#include "purgeable.c"

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
	expect(isPurgeableUnsupported("hfs"), "the HFSX data volume has no purgeable state");
	expect(isPurgeableUnsupported("devfs"), "nor any other non-APFS volume");
	expect(!isPurgeableUnsupported("apfs"), "APFS keeps Apple's call");
	expect(!isPurgeableUnsupported(NULL), "an unknown volume keeps Apple's call");
	expect(!isPurgeableUnsupported(""), "an empty type keeps Apple's call");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
