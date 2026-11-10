/*
 * init26: the real-time clock's offset (kernel/drivers/rtc, boot-arg rtc=1), kept on the data volume.
 *
 * The PMK8350's counter is not the time: the time is the counter plus an offset. The kernel knows it while it runs
 * (kern.hakios.rtc_offset: the boot-arg rtc_offset=, or the time the system last set), and init26 keeps it in
 * /private/var/db/dlisa-rtc-offset between boots: written to the kernel early in the boot, saved again by a small
 * keeper whenever the kernel's changes. The file is one line, the offset in seconds ("-1762800000\n").
 *
 * Pure string work, no system calls: init-26.c includes it after mini.h, test/rtc-offset-test.c after <string.h>.
 */

/* the kernel's kern.hakios.rtc_source values (drivers/rtc/dlisa.rtc.c) */
#define RTC_SOURCE_BOOTARG      3
#define RTC_SOURCE_SAVED        4
#define RTC_SOURCE_SET          5

/* The offset in TEXT (LENGTH bytes, not a string): a sign, 1 to 18 digits, an optional line end. 1, or 0 (and *offset as it was). */
static int
parseRtcOffset(const char *text, size_t length, long long *offset)
{
	size_t at = 0, digits = 0;
	long long value = 0;
	int negative = 0;

	while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
		length--;
	}
	if (at < length && text[at] == '-') {
		negative = 1;
		at++;
	}
	for (; at < length && text[at] >= '0' && text[at] <= '9' && digits < 18; at++, digits++) {
		value = value * 10 + (text[at] - '0');
	}
	if (digits == 0 || at != length) {
		return 0;
	}
	*offset = negative ? -value : value;
	return 1;
}

/* OFFSET and a newline into OUT (SIZE bytes, NUL-terminated); its length, or 0 when it does not fit */
static size_t
formatRtcOffset(char *out, size_t size, long long offset)
{
	char digits[24];
	size_t count = 0, at = 0;
	unsigned long long rest = offset < 0 ? 0ULL - (unsigned long long)offset : (unsigned long long)offset;

	do {
		digits[count++] = (char)('0' + rest % 10);
		rest /= 10;
	} while (rest != 0);
	if ((offset < 0) + count + 2 > size) {
		return 0;
	}
	if (offset < 0) {
		out[at++] = '-';
	}
	while (count > 0) {
		out[at++] = digits[--count];
	}
	out[at++] = '\n';
	out[at] = 0;
	return at;
}

/* An offset worth keeping for the next boot: one that was given or set, not the kernel's estimate */
static int
isRtcOffsetToKeep(int source)
{
	return source == RTC_SOURCE_BOOTARG || source == RTC_SOURCE_SAVED || source == RTC_SOURCE_SET;
}
