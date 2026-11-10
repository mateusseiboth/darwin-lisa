/*
 * Host test of ld-disable.c (init26's boot-arg ld_disable=): cc -I.. -o /tmp/t ld-disable-test.c && /tmp/t
 * test/run-tests.sh does that.
 */
#include <stdio.h>
#include <string.h>

#include "ld-disable.c"

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
hasKey(const char *plist, const char *label, const char *value)
{
	char line[256];

	snprintf(line, sizeof(line), "\t<key>%s</key>\n\t<%s/>\n", label, value);
	return strstr(plist, line) != NULL;
}

static int
countOf(const char *plist, const char *label)
{
	char key[256];
	int n = 0;

	snprintf(key, sizeof(key), "<key>%s</key>", label);
	for (const char *p = plist; (p = strstr(p, key)) != NULL; p++) {
		n++;
	}
	return n;
}

static const char GROUPS[] =
    "# comment line\n"
    "ui com.apple.ClarityBoard com.apple.checkerboard\n"
    "\n"
    "lean2 com.apple.locationd   com.apple.gpsd\n";

static void
testWithoutBootArgKeepsLaunchdDefaults(void)
{
	char out[4096];
	int n = buildDisabledPlist("rd=md0 cpus=7", GROUPS, out, sizeof(out));

	expect(n == 0, "no ld_disable: nothing of ours");
	expect(hasKey(out, "com.apple.dhcp6d", "true"), "no ld_disable: dhcp6d stays disabled");
	expect(hasKey(out, "com.apple.bootpd", "true"), "no ld_disable: bootpd stays disabled");
	expect(hasKey(out, "com.apple.ftp-proxy-embedded", "false"), "no ld_disable: ftp-proxy stays enabled");
	expect(strncmp(out, "<?xml", 5) == 0, "no ld_disable: an XML plist");
	expect(strstr(out, "</dict>\n</plist>\n") != NULL, "no ld_disable: a closed plist");
}

static void
testShortLabelGetsTheApplePrefix(void)
{
	char out[4096];
	int n = buildDisabledPlist("a=1 ld_disable=locationd b=2", GROUPS, out, sizeof(out));

	expect(n == 1, "short label: one");
	expect(hasKey(out, "com.apple.locationd", "true"), "short label: com.apple.locationd");
	expect(countOf(out, "b=2") == 0, "short label: the value ends at the space");
}

static void
testGroupsAndFullLabels(void)
{
	char out[4096];
	int n = buildDisabledPlist("ld_disable=ui,lean2,com.example.x", GROUPS, out, sizeof(out));

	expect(n == 5, "groups: five labels");
	expect(hasKey(out, "com.apple.ClarityBoard", "true"), "groups: ClarityBoard");
	expect(hasKey(out, "com.apple.checkerboard", "true"), "groups: checkerboard");
	expect(hasKey(out, "com.apple.locationd", "true"), "groups: locationd");
	expect(hasKey(out, "com.apple.gpsd", "true"), "groups: gpsd");
	expect(hasKey(out, "com.example.x", "true"), "groups: a full label as it is");
}

static void
testDuplicatesAreWrittenOnce(void)
{
	char out[4096];
	int n = buildDisabledPlist("ld_disable=dhcp6d,ui,checkerboard,ftp-proxy-embedded", GROUPS, out, sizeof(out));

	expect(n == 4, "duplicates: four distinct labels");
	expect(countOf(out, "com.apple.dhcp6d") == 1, "duplicates: dhcp6d once");
	expect(countOf(out, "com.apple.checkerboard") == 1, "duplicates: checkerboard once");
	expect(hasKey(out, "com.apple.ftp-proxy-embedded", "true"), "duplicates: a default turned off");
	expect(countOf(out, "com.apple.ftp-proxy-embedded") == 1, "duplicates: ftp-proxy once");
}

static void
testOnlyAWholeWordCounts(void)
{
	char out[4096];
	int n = buildDisabledPlist("xld_disable=locationd", GROUPS, out, sizeof(out));

	expect(n == 0, "whole word: xld_disable= is not ld_disable=");
}

static void
testWithoutGroupsFile(void)
{
	char out[4096];
	int n = buildDisabledPlist("ld_disable=ui", NULL, out, sizeof(out));

	expect(n == 1, "no groups file: the token is a short label");
	expect(hasKey(out, "com.apple.ui", "true"), "no groups file: com.apple.ui");
}

static const char GROUPS_WITH_DEFAULT[] =
    "default com.apple.CommCenter\n"
    "ui com.apple.ClarityBoard\n";

static void
testDefaultGroupIsAlwaysDisabled(void)
{
	char out[4096];
	int n = buildDisabledPlist("rd=md0", GROUPS_WITH_DEFAULT, out, sizeof(out));

	expect(n == 1, "default group: one without the boot-arg");
	expect(hasKey(out, "com.apple.CommCenter", "true"), "default group: CommCenter disabled");
}

static void
testDefaultGroupAddsToTheBootArg(void)
{
	char out[4096];
	int n = buildDisabledPlist("ld_disable=ui", GROUPS_WITH_DEFAULT, out, sizeof(out));

	expect(n == 2, "default group + ui: two");
	expect(hasKey(out, "com.apple.CommCenter", "true"), "default group + ui: CommCenter");
	expect(hasKey(out, "com.apple.ClarityBoard", "true"), "default group + ui: ClarityBoard");
}

static void
testDefaultGroupLeftOutOnRequest(void)
{
	char out[4096];
	int n = buildDisabledPlist("ld_default=0 ld_disable=ui", GROUPS_WITH_DEFAULT, out, sizeof(out));

	expect(n == 1, "ld_default=0: only the boot-arg's");
	expect(countOf(out, "com.apple.CommCenter") == 0, "ld_default=0: CommCenter left alone");
}

static void
testTooSmallABuffer(void)
{
	char out[64];

	expect(buildDisabledPlist("ld_disable=ui", GROUPS, out, sizeof(out)) == -1, "small buffer: refused");
}

int
main(void)
{
	testWithoutBootArgKeepsLaunchdDefaults();
	testShortLabelGetsTheApplePrefix();
	testGroupsAndFullLabels();
	testDuplicatesAreWrittenOnce();
	testOnlyAWholeWordCounts();
	testWithoutGroupsFile();
	testDefaultGroupIsAlwaysDisabled();
	testDefaultGroupAddsToTheBootArg();
	testDefaultGroupLeftOutOnRequest();
	testTooSmallABuffer();
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
