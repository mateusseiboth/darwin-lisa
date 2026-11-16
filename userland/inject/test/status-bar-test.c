/* Host test of status-bar.c (libdlisa: which split status bar UIKit is told to use, from the display's width) */
#include <stdio.h>

#include "status-bar.c"

static int failures;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static int
isNamed(const char *got, const char *want)
{
	return got != NULL && want != NULL ? strcmp(got, want) == 0 : got == want;
}

int
main(void)
{
	(void)isNamedByBootArg;   /* name-list.c's other half, used by libdlisa itself */
	expect(isNamed(findStatusBarProvider("gpu=1 cpus=4"), "_UIStatusBarVisualProvider_Split1125"),
	    "no fbwidth: the kernel's 1125 x 2436, the iPhone X's split bar");
	expect(isNamed(findStatusBarProvider("fbwidth=1125 fbheight=2436"), "_UIStatusBarVisualProvider_Split1125"),
	    "fbwidth=1125 as well");
	expect(isNamed(findStatusBarProvider("fbwidth=1080 fbheight=2400"), "_UIStatusBarVisualProvider_Split1080_Wide"),
	    "the panel's own 1080: the 13 mini's");
	expect(isNamed(findStatusBarProvider("fbwidth=1170 fbheight=2532"), "_UIStatusBarVisualProvider_Split1170"),
	    "1170: the iPhone 12's");
	expect(isNamed(findStatusBarProvider("fbwidth=828 fbheight=1792 fbscale=2"), "_UIStatusBarVisualProvider_Split828"),
	    "828: the XR's");
	expect(findStatusBarProvider("fbwidth=1000") == NULL, "a width no split bar has: none");
	expect(findStatusBarProvider("fbwidth=1080 dlisa_sbprov=0") == NULL, "dlisa_sbprov=0: none, UIKit chooses");
	expect(isNamed(findStatusBarProvider("dlisa_sbprov=1"), "_UIStatusBarVisualProvider_Split1125"),
	    "dlisa_sbprov=1 is the default");
	expect(isNamed(findStatusBarProvider("xfbwidth=1080"), "_UIStatusBarVisualProvider_Split1125"),
	    "a boot-arg that only ends with fbwidth is another one");
	expect(isNamed(findStatusBarProvider(NULL), "_UIStatusBarVisualProvider_Split1125"), "no boot-args: the default");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
