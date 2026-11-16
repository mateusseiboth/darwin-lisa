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
isNear(double got, double want)
{
	return got > want - 0.005 && got < want + 0.005;
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
	expect(isNamed(findStatusBarProvider("cpus=4 lisa=1 gpu=1"), "_UIStatusBarVisualProvider_Split1080_Wide"),
	    "lisa=1: the kernel's built-in fbwidth=1080 fbheight=2400, the 13 mini's split bar");
	expect(isNamed(findStatusBarProvider("lisa=1 fbwidth=1125 fbheight=2436"), "_UIStatusBarVisualProvider_Split1125"),
	    "an fbwidth on the line wins over lisa=1's");
	expect(isNamed(findStatusBarProvider("lisa=0"), "_UIStatusBarVisualProvider_Split1125"), "lisa=0: the kernel's 1125");
	expect(isNamed(findStatusBarProviderForWidth(1080), "_UIStatusBarVisualProvider_Split1080_Wide"),
	    "by the width UIScreen reports: 1080");
	expect(findStatusBarProviderForWidth(1000) == NULL, "by a width no split bar has: none");
	expect(findStatusBarProvider("fbwidth=1000") == NULL, "a width no split bar has: none");
	expect(findStatusBarProvider("fbwidth=1080 dlisa_sbprov=0") == NULL, "dlisa_sbprov=0: none, UIKit chooses");
	expect(isNamed(findStatusBarProvider("dlisa_sbprov=1"), "_UIStatusBarVisualProvider_Split1125"),
	    "dlisa_sbprov=1 is the default");
	expect(isNamed(findStatusBarProvider("xfbwidth=1080"), "_UIStatusBarVisualProvider_Split1125"),
	    "a boot-arg that only ends with fbwidth is another one");
	expect(isNamed(findStatusBarProvider(NULL), "_UIStatusBarVisualProvider_Split1125"), "no boot-args: the default");
	expect(readDisplayWidth("lisa=1 cpus=4") == 1080, "the display's width: lisa=1's 1080");
	expect(readDisplayWidth("fbwidth=1125") == 1125, "the display's width: fbwidth=");
	expect(readDisplayWidth("gpu=1") == 1125, "the display's width: the kernel's own");
	expect(readDisplayScale("lisa=1") == 3, "the display's scale: 3");
	expect(readDisplayScale("fbscale=2") == 2, "the display's scale: fbscale=");
	{
		/* Split1080_Wide: notch 174 x 37.33 points at a reference scale of 2.88, on 1080 pixels at 3 (360 points) */
		DlisaNotch n = computeSplitNotch(1080, 3, 174, 37.0 + 1.0 / 3, 2.88);

		expect(isNear(n.width, 167.04) && isNear(n.height, 35.84), "the 13 mini's notch at 3 instead of 2.88");
		expect(isNear(n.x, 96.48) && n.y == 0, "centred at the top of 360 points");
	}
	{
		/* Split1125 has no reference scale of its own: the screen's; on the iPhone X's 1125 at 3 it is the X's notch */
		DlisaNotch n = computeSplitNotch(1125, 3, 209, 30, 0);

		expect(isNear(n.x, 83) && isNear(n.width, 209) && isNear(n.height, 30), "the X's notch, as SpringBoard's type 3");
	}
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
