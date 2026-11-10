/* Host test of wifi-net.c (init26: the network the kernel joins, and wifid only on a Wi-Fi boot) */
#include <stdio.h>
#include <string.h>

#include "wifi-net.c"

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
valid(const char *text)
{
	return isWifiNetworkText(text, strlen(text));
}

static int
argsAre(const char *bootArgs, const char *want)
{
	char out[256];
	const char *got = buildWifiBootArgs(bootArgs, out, sizeof(out));

	if (got == NULL || strcmp(got, want) != 0) {
		printf("  got \"%s\"\n  want \"%s\"\n", got ? got : "(null)", want);
		return 0;
	}
	return 1;
}

int
main(void)
{
	char small[24];

	expect(valid("Home\npassword1\n"), "a name and a passphrase");
	expect(valid("Home\r\npassword1\r\n"), "Windows line ends");
	expect(valid("Home\npassword1"), "no newline at the end");
	expect(valid("Cafe\n"), "a name alone: an open network");
	expect(valid("Cafe"), "a name alone without a newline");
	expect(valid("0123456789abcdef0123456789abcdef\n0123456789012345678901234567890123456789012345678901234567890ab\n"),
	    "a name of 32 bytes and a passphrase of 63");
	expect(!valid(""), "empty: no network");
	expect(!valid("\npassword1\n"), "no name: no network");
	expect(!valid("Home\nshort\n"), "a passphrase under 8 bytes: no network");
	expect(!valid("0123456789abcdef0123456789abcdef0\npassword1\n"), "a name of 33 bytes: no network");
	expect(!valid("Home\n0123456789012345678901234567890123456789012345678901234567890abc\n"),
	    "a passphrase of 64 bytes (a raw key): no network");
	expect(!valid("Home\npassword1\nmore\n"), "a third line: no network");

	expect(argsAre("rd=md0 wifi=1", "rd=md0 wifi=1"), "wifi=1: wifid stays");
	expect(argsAre("rd=md0 wifi=1 ld_disable=lean1", "rd=md0 wifi=1 ld_disable=lean1"), "wifi=1 with ld_disable: unchanged");
	expect(argsAre("rd=md0", "rd=md0 ld_disable=wifid"), "no wifi=: wifid off");
	expect(argsAre("rd=md0 wifi=0", "rd=md0 wifi=0 ld_disable=wifid"), "wifi=0: wifid off");
	expect(argsAre("rd=md0 wifi_step=14", "rd=md0 wifi_step=14 ld_disable=wifid"), "wifi_step= is not wifi=: wifid off");
	expect(argsAre("wifi=1 wifid=0", "wifi=1 wifid=0 ld_disable=wifid"), "wifid=0: off even with wifi=1");
	expect(argsAre("rd=md0 ld_disable=lean1,ui cpus=7", "rd=md0 ld_disable=wifid,lean1,ui cpus=7"),
	    "an ld_disable= there: wifid goes in front of its list");
	expect(argsAre("", "ld_disable=wifid"), "no boot-args at all: wifid off");
	expect(buildWifiBootArgs("rd=md0 ufs=2 ufs_part=super", small, sizeof(small)) != NULL &&
	    strcmp(buildWifiBootArgs("rd=md0 ufs=2 ufs_part=super", small, sizeof(small)), "rd=md0 ufs=2 ufs_part=super") == 0,
	    "no room for the change: the boot-args as they were");
	expect(strcmp(withWifiServices("rd=md0 cpus=7"), "rd=md0 cpus=7 ld_disable=wifid") == 0,
	    "withWifiServices: its own buffer, the same change");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
