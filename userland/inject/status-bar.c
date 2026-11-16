/*
 * libdlisa: which split status bar fits this display, and the notch it wants (docs/home-button-type.md, part 5).
 *
 * UIKit takes a split bar when its native width and its notch match the screen's, and found none for this phone
 * ("rdar:45025538" in the status bar). The display iOS is told about is the kernel's: fbwidth= on the line, else
 * lisa=1's built-in fbwidth=1080 (kernel/drivers/bootargs/lisa-defaults-wire.py), else the kernel's own 1125, at
 * fbscale= (default 3) pixels a point. At 1080 pixels it is the 13 mini's Split1080_Wide; SpringBoard then hands
 * every process the notch that bar wants (computeSplitNotch) and UIKit picks it by itself. `line` is kern.bootargs.
 */
#include <stdlib.h>
#include <string.h>

#include "name-list.c"

#define FRAMEBUFFER_DEFAULT_WIDTH 1125L
#define FRAMEBUFFER_LISA_WIDTH    1080L         /* lisa=1 */
#define FRAMEBUFFER_DEFAULT_SCALE 3L

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

/* the display's width in pixels: fbwidth= on the line, else lisa=1's 1080, else the kernel's 1125 */
static long
readDisplayWidth(const char *line)
{
	long kernelWidth = readBootArgNumber(line, "lisa", 0) == 1 ? FRAMEBUFFER_LISA_WIDTH : FRAMEBUFFER_DEFAULT_WIDTH;

	return readBootArgNumber(line, "fbwidth", kernelWidth);
}

/* pixels a point: fbscale= on the line, else 3 */
static long
readDisplayScale(const char *line)
{
	return readBootArgNumber(line, "fbscale", FRAMEBUFFER_DEFAULT_SCALE);
}

/* the class name, or NULL: dlisa_sbprov=0, or a width no split bar is made for (UIKit then chooses) */
static const char *
findStatusBarProvider(const char *line)
{
	if (readBootArgNumber(line, "dlisa_sbprov", 1) == 0) {
		return NULL;
	}
	return findStatusBarProviderForWidth(readDisplayWidth(line));
}

/*
 * The notch a split bar wants to see, in the screen's points: UIKit takes a split bar when its +notchSize equals the
 * exclusion area times (native scale / its +referenceScreenScale, or the screen's scale when it has none). On a
 * display that is not scaled the native scale is the scale. Centred at the top, as SpringBoard draws its notches.
 */
typedef struct {
	double x, y, width, height;
} DlisaNotch;

static DlisaNotch
computeSplitNotch(double nativeWidth, double scale, double notchWidth, double notchHeight, double referenceScale)
{
	double ratio = (referenceScale > 0 ? referenceScale : scale) / scale;
	DlisaNotch notch = { 0, 0, notchWidth * ratio, notchHeight * ratio };

	notch.x = (nativeWidth / scale - notch.width) / 2;
	return notch;
}
