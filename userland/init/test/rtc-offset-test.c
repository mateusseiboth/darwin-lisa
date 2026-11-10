/* Host test of rtc-offset.c (init26: the real-time clock's offset kept on the data volume) */
#include <stdio.h>
#include <string.h>

#include "rtc-offset.c"

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
parses(const char *text, long long want)
{
	long long got = 12345;

	return parseRtcOffset(text, strlen(text), &got) && got == want;
}

static int
refuses(const char *text)
{
	long long got = 12345;

	return !parseRtcOffset(text, strlen(text), &got) && got == 12345;
}

int
main(void)
{
	char out[32];

	expect(parses("1762800000\n", 1762800000LL), "a positive offset with its newline");
	expect(parses("-42", -42), "a negative offset without a newline");
	expect(parses("0\r\n", 0), "zero, with a Windows line end");
	expect(parses("1748099000", 1748099000LL), "a count-sized offset");
	expect(refuses(""), "empty: no offset");
	expect(refuses("\n"), "a lone newline: no offset");
	expect(refuses("-"), "a lone sign: no offset");
	expect(refuses("12a"), "a letter: no offset");
	expect(refuses("1 2"), "a space inside: no offset");
	expect(refuses("99999999999999999999"), "too many digits: no offset");
	expect(refuses("12\n34"), "a second line: no offset");

	expect(formatRtcOffset(out, sizeof(out), -1762800000LL) == 12 && strcmp(out, "-1762800000\n") == 0,
	    "format: a negative offset and its newline");
	expect(formatRtcOffset(out, sizeof(out), 0) == 2 && strcmp(out, "0\n") == 0, "format: zero");
	expect(formatRtcOffset(out, 4, 123456) == 0, "format: no room, nothing");
	formatRtcOffset(out, sizeof(out), 9876543210LL);
	expect(parses(out, 9876543210LL), "format then parse: the same offset");

	expect(!isRtcOffsetToKeep(0) && !isRtcOffsetToKeep(1) && !isRtcOffsetToKeep(2), "estimates are not kept");
	expect(isRtcOffsetToKeep(3) && isRtcOffsetToKeep(4) && isRtcOffsetToKeep(5), "rtc_offset=, saved and set are kept");
	expect(!isRtcOffsetToKeep(6) && !isRtcOffsetToKeep(-1), "an unknown source is not kept");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
