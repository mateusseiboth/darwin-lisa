/*
 * libdlisa: which split status bar UIKit is told to use (SpringBoard writes it to com.apple.UIKit's
 * UIStatusBarVisualProviderClassName, a default UIKit reads for the embedded screen before it chooses by itself).
 *
 * UIKit chooses a split bar by the display's native width and the notch SpringBoard gives it, and found none for this
 * phone ("rdar:45025538" in the status bar, docs/home-button-type.md). The display iOS is told about is the kernel's
 * (fbwidth=, default 1125): the split bar made for that width. `line` is kern.bootargs as it is.
 */
#include <stdlib.h>
#include <string.h>

#include "name-list.c"

#define FRAMEBUFFER_DEFAULT_WIDTH 1125L

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

/* the class name, or NULL: dlisa_sbprov=0, or a width no split bar is made for (UIKit then chooses) */
static const char *
findStatusBarProvider(const char *line)
{
	long width = readBootArgNumber(line, "fbwidth", FRAMEBUFFER_DEFAULT_WIDTH);

	if (readBootArgNumber(line, "dlisa_sbprov", 1) == 0) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(SPLIT_PROVIDERS) / sizeof(SPLIT_PROVIDERS[0]); i++) {
		if (SPLIT_PROVIDERS[i].width == width) {
			return SPLIT_PROVIDERS[i].provider;
		}
	}
	return NULL;
}
