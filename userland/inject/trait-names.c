/*
 * libdlisa, dlisa_uiprobe's text lines: UIUserInterfaceStyle and a colour as words. Under the HBT identity Settings'
 * text is drawn near-white on its light background (status bar "rdar:45025538": 539 near-white pixels and 8 dark ones,
 * runs S11-A and int1-b; S9, classic identity: 785 dark). These lines say whether the app resolves its label colours
 * for a dark appearance or the render server changes them.
 */
#include <stddef.h>
#include <stdio.h>

static const char *
nameInterfaceStyle(long style)
{
	static const char *const names[] = { "unspecified", "light", "dark" };

	return style >= 0 && style < 3 ? names[style] : "?";
}

static int
clampByte(double component)
{
	double scaled = component * 255.0 + 0.5;

	return scaled < 0 ? 0 : scaled > 255 ? 255 : (int)scaled;
}

static void
formatRgba(char *text, size_t size, double r, double g, double b, double a)
{
	snprintf(text, size, "rgba(%d,%d,%d,%.2f)", clampByte(r), clampByte(g), clampByte(b), a);
}
