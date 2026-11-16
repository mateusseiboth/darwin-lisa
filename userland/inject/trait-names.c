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

/*
 * The second probe (ctl2, 2026-11-16): the window is light and labelColor black, yet neither the "Settings" title (a
 * black UILabel) nor the SwiftUI rows show, while "Search" inside the glass does. So each label and each layer that can
 * hold text says where it is, its size and font, and whether it has contents at all.
 */
#include <stdbool.h>
#include <string.h>

/* "(x,y wxh)": whole numbers as such, else one decimal */
static void
formatRect(char *text, size_t size, double x, double y, double w, double h)
{
	snprintf(text, size, "(%.*f,%.*f %.*fx%.*f)", x == (long)x ? 0 : 1, x, y == (long)y ? 0 : 1, y, w == (long)w ? 0 : 1, w,
	    h == (long)h ? 0 : 1, h);
}

static bool
isRectEmpty(double w, double h)
{
	return w <= 0 || h <= 0;
}

/* a layer class that draws text or SwiftUI content: UIKit's label layers, CATextLayer, RenderBox, SwiftUI's CG drawing */
static bool
isTextLayerClassName(const char *name)
{
	static const char *const marks[] = { "Label", "Text", "RBLayer", "RenderBox", "CGDrawing" };

	if (name == NULL) {
		return false;
	}
	for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
		if (strstr(name, marks[i]) != NULL) {
			return true;
		}
	}
	return false;
}
