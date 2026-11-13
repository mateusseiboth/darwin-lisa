/* Host test of name-list.c (libdlisa: is this program named by a boot-arg such as dlisa_oslog= or dlisa_uiprobe=) */
#include <stdio.h>

#include "name-list.c"

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
	const char *line = "gpu=1 dlisa_oslog=PosterBoard,CollectionsPoster dlisa_uiprobe=Preferences,SpringBoard cpus=4";

	expect(isNamedByBootArg(line, "dlisa_uiprobe", "Preferences"), "the first name of the list");
	expect(isNamedByBootArg(line, "dlisa_uiprobe", "SpringBoard"), "the last name of the list");
	expect(!isNamedByBootArg(line, "dlisa_uiprobe", "Pref"), "a prefix of a name is not the name");
	expect(!isNamedByBootArg(line, "dlisa_uiprobe", "PosterBoard"), "a name of another boot-arg's list");
	expect(isNamedByBootArg(line, "dlisa_oslog", "CollectionsPoster"), "the same for dlisa_oslog");
	expect(!isNamedByBootArg(line, "dlisa_scroll", "backboardd"), "a boot-arg that is not there names nobody");
	expect(isNamedByBootArg("dlisa_uiprobe=all", "dlisa_uiprobe", "Preferences"), "all names everybody");
	expect(isNamedByBootArg("x=1 dlisa_oslog=1", "dlisa_oslog", "lsd"), "and so does 1");
	expect(!isNamedByBootArg("xdlisa_uiprobe=Preferences", "dlisa_uiprobe", "Preferences"),
	    "a boot-arg whose name only ends with the key is another boot-arg");
	expect(!isNamedByBootArg("dlisa_uiprobe_n=Preferences", "dlisa_uiprobe", "Preferences"),
	    "nor one whose name only starts with it");
	expect(!isNamedByBootArg(NULL, "dlisa_uiprobe", "Preferences"), "no boot-args name nobody");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
