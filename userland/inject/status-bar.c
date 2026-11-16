/*
 * libdlisa: which split status bar UIKit is told to use (SpringBoard writes it to com.apple.UIKit's
 * UIStatusBarVisualProviderClassName, a default UIKit reads for the embedded screen before it chooses by itself).
 *
 * UIKit chooses a split bar by the display's native width and the notch SpringBoard gives it, and found none for this
 * phone ("rdar:45025538" in the status bar, docs/home-button-type.md). The display iOS is told about is the kernel's:
 * fbwidth= on the line, else lisa=1's built-in fbwidth=1080 (kernel/drivers/bootargs/lisa-defaults-wire.py), else the
 * kernel's own 1125. The split bar made for that width: at 1080 pixels and a native scale of 3 it is the 13 mini's
 * Split1080_Wide (the notch it wants is the 13 mini's that SpringBoard gives at 375 x 812, divided by 3 / 2.88).
 * `line` is kern.bootargs as it is.
 */
#include <stdlib.h>
#include <string.h>

#include "name-list.c"

#define FRAMEBUFFER_DEFAULT_WIDTH 1125L
#define FRAMEBUFFER_LISA_WIDTH    1080L         /* lisa=1 */

static const struct {
	long width;
	const char *provider;
} SPLIT_PROVIDERS[] = {
	{ 1125, "_UIStatusBarVisualProvider_Split1125" },       /* iPhone X, XS, 11 Pro: 375 points at 3 */
	{ 1080, "_UIStatusBarVisualProvider_Split1080_Wide" },  /* iPhone 13 mini */
	{ 1170, "_UIStatusBarVisualProvider_Split1170" },       /* iPhone 12, 13 */
	{ 1242, "_UIStatusBarVisualProvider_Split1242" },       /* iPhone XS Max, 11 Pro Max */
	{ 1284, "_UIStatusBarVisualProvider_Split1284" },       /* iPhone 12 Pro Max */
	{ 828, "_UIStatusBarVisualProvider_Split828" },         /* iPhone XR, 11 */
};

static long
readBootArgNumber(const char *line, const char *key, long fallback)
{
	const char *value = line != NULL ? findBootArgValue(line, key) : NULL;

	return value != NULL ? strtol(value, NULL, 10) : fallback;
}

/* the split bar made for a display this many pixels wide, or NULL */
static const char *
findStatusBarProviderForWidth(long width)
{
	for (size_t i = 0; i < sizeof(SPLIT_PROVIDERS) / sizeof(SPLIT_PROVIDERS[0]); i++) {
		if (SPLIT_PROVIDERS[i].width == width) {
			return SPLIT_PROVIDERS[i].provider;
		}
	}
	return NULL;
}

/* the class name, or NULL: dlisa_sbprov=0, or a width no split bar is made for (UIKit then chooses) */
static const char *
findStatusBarProvider(const char *line)
{
	long kernelWidth = readBootArgNumber(line, "lisa", 0) == 1 ? FRAMEBUFFER_LISA_WIDTH : FRAMEBUFFER_DEFAULT_WIDTH;

	if (readBootArgNumber(line, "dlisa_sbprov", 1) == 0) {
		return NULL;
	}
	return findStatusBarProviderForWidth(readBootArgNumber(line, "fbwidth", kernelWidth));
}
