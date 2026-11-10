/* Host test of time-zone.c (init26: boot-arg tz=, the system's time zone kept on the data volume) */
#include <stdio.h>
#include <string.h>

#include "time-zone.c"

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
	char out[128];

	expect(isTimeZoneName("America/Campo_Grande"), "an area and a city");
	expect(isTimeZoneName("UTC"), "a single word");
	expect(isTimeZoneName("Etc/GMT+4"), "a sign and a digit");
	expect(isTimeZoneName("America/Argentina/Buenos_Aires"), "three parts");
	expect(isTimeZoneName("America/Port-au-Prince"), "hyphens");
	expect(!isTimeZoneName(""), "empty: no zone");
	expect(!isTimeZoneName("/etc/passwd"), "an absolute path: no zone");
	expect(!isTimeZoneName("../../etc/passwd"), "a way up: no zone");
	expect(!isTimeZoneName("America/../UTC"), "a way up inside: no zone");
	expect(!isTimeZoneName("America//Cuiaba"), "an empty part: no zone");
	expect(!isTimeZoneName("America/"), "a trailing slash: no zone");
	expect(!isTimeZoneName("America/Campo Grande"), "a space: no zone");
	memset(out, 'A', 64);
	out[64] = 0;
	expect(!isTimeZoneName(out), "64 bytes: too long");
	out[63] = 0;
	expect(isTimeZoneName(out), "63 bytes: the longest");

	expect(buildTimeZoneLink(out, sizeof(out), "America/Campo_Grande") == strlen(TZ_LINK_BASE "America/Campo_Grande") &&
	    strcmp(out, "/var/db/timezone/zoneinfo/America/Campo_Grande") == 0, "the link goes through the zoneinfo link");
	expect(buildTimeZoneLink(out, 30, "America/Campo_Grande") == 0, "no room: nothing");

	expect(isTimeZoneLinkTo("/var/db/timezone/zoneinfo/America/Campo_Grande", 46, "America/Campo_Grande"),
	    "the link already names the zone");
	expect(!isTimeZoneLinkTo("/var/db/timezone/zoneinfo/US/Pacific", 36, "America/Campo_Grande"),
	    "the skeleton's US/Pacific is another zone");
	expect(!isTimeZoneLinkTo("/var/db/timezone/zoneinfo/America/Campo_Grande_X", 48, "America/Campo_Grande"),
	    "a longer name is another zone");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
