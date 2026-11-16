/* Host test of trait-names.c (libdlisa's dlisa_uiprobe text lines: UIUserInterfaceStyle and a colour, as words) */
#include <stdio.h>
#include <string.h>

#include "trait-names.c"

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
	char text[64];

	expect(strcmp(nameInterfaceStyle(0), "unspecified") == 0, "0 is unspecified");
	expect(strcmp(nameInterfaceStyle(1), "light") == 0, "1 is light");
	expect(strcmp(nameInterfaceStyle(2), "dark") == 0, "2 is dark");
	expect(strcmp(nameInterfaceStyle(7), "?") == 0, "anything else is unknown");
	formatRgba(text, sizeof(text), 0, 0, 0, 1);
	expect(strcmp(text, "rgba(0,0,0,1.00)") == 0, "black as 0..255 and alpha");
	formatRgba(text, sizeof(text), 1, 1, 1, 0.5);
	expect(strcmp(text, "rgba(255,255,255,0.50)") == 0, "white, half transparent");
	formatRgba(text, sizeof(text), 1.2, -0.1, 0.5, 1);
	expect(strcmp(text, "rgba(255,0,128,1.00)") == 0, "extended-range components are clamped");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
