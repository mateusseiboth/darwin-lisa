/*
 * Host test of lisa-defaults-wire.py: pexpert/gen/bootargs.c as the wire leaves it, compiled with stand-in headers
 * (run-tests.sh). The kernel's command line is `line` here.
 *
 * What must hold:
 *   - without lisa=1 (or with lisa=0) the defaults are never seen: a host run's line means what it meant;
 *   - with lisa=1 an argument missing from the line takes lisa's default, number or string;
 *   - an argument on the line always wins over the default;
 *   - flags and arguments the defaults do not name behave as before.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pexpert/pexpert.h>

static char line[1024];
static int failures;

char *
PE_boot_args(void)
{
	return line;
}

int
IODTGetDefault(const char *key, void *infoAddr, unsigned int infoSize)
{
	(void)key;
	(void)infoAddr;
	(void)infoSize;
	return -1;
}

static void
useLine(const char *text)
{
	snprintf(line, sizeof(line), "%s", text);
}

static void
expectNumber(const char *what, const char *name, int found, uint32_t value)
{
	uint32_t got = 0xdeadbeef;
	int was = PE_parse_boot_argn(name, &got, sizeof(got)) ? 1 : 0;

	if (was != found || (found && got != value)) {
		printf("FAIL %s: %s found %d value %u (wanted found %d value %u) on \"%s\"\n", what, name, was, got, found,
		    value, line);
		failures++;
	}
}

static void
expectString(const char *what, const char *name, int found, const char *value)
{
	char got[64] = "untouched";
	int was = PE_parse_boot_arg_str(name, got, sizeof(got)) ? 1 : 0;

	if (was != found || (found && strcmp(got, value) != 0)) {
		printf("FAIL %s: %s found %d value \"%s\" (wanted found %d value \"%s\") on \"%s\"\n", what, name, was, got,
		    found, found ? value : "", line);
		failures++;
	}
}

int
main(void)
{
	char chip[16] = "";

	useLine("rd=md0 cpus=7 display=1");
	expectNumber("no lisa=1", "touchscreen", 0, 0);
	expectNumber("no lisa=1", "cpufreq_max", 0, 0);
	expectString("no lisa=1", "wifi_mac", 0, NULL);
	expectNumber("no lisa=1: the line as before", "cpus", 1, 7);

	useLine("rd=md0 lisa=0 display=1");
	expectNumber("lisa=0", "touchscreen", 0, 0);
	expectNumber("lisa=0", "fbwidth", 0, 0);

	useLine("rd=md0 lisa=1 display=1 cpus=7");
	expectNumber("lisa=1", "touchscreen", 1, 1);
	expectNumber("lisa=1", "touch_goodix_a", 1, 1);
	expectNumber("lisa=1", "cpufreq", 1, 1);
	expectNumber("lisa=1", "cpufreq_max", 1, 2400);
	expectNumber("lisa=1", "fbwidth", 1, 1080);
	expectNumber("lisa=1", "fbheight", 1, 2400);
	expectNumber("lisa=1: zero is a value", "wifi_irq", 1, 0);
	expectString("lisa=1", "touch_avdd_ldo", 1, "ldoc7");
	expectString("lisa=1", "touch_chip", 1, "goodix");
	expectString("lisa=1", "wifi_mac", 1, "02:6c:69:73:61:01");
	expectNumber("lisa=1: the line's own", "display", 1, 1);
	expectNumber("lisa=1: the line's own", "cpus", 1, 7);
	expectNumber("lisa=1: not a default", "wifi", 0, 0);
	expectNumber("lisa=1: not a default", "adsp", 0, 0);
	expectNumber("lisa=1: a flag not given", "-v", 0, 0);
	/* touch_chip is read with PE_parse_boot_argn into a character buffer (hakios.goodix.c) */
	if (!PE_parse_boot_argn("touch_chip", chip, sizeof(chip)) || strcmp(chip, "goodix") != 0) {
		printf("FAIL lisa=1: touch_chip through PE_parse_boot_argn gave \"%s\"\n", chip);
		failures++;
	}

	useLine("lisa=1 touchscreen=0 fbwidth=1125 fbheight=2499 wifi_irq=1 wifi_mac=02:00:00:00:00:02 -v");
	expectNumber("the line wins", "touchscreen", 1, 0);
	expectNumber("the line wins", "fbwidth", 1, 1125);
	expectNumber("the line wins", "fbheight", 1, 2499);
	expectNumber("the line wins", "wifi_irq", 1, 1);
	expectString("the line wins", "wifi_mac", 1, "02:00:00:00:00:02");
	expectNumber("the line wins, the rest is lisa's", "cpufreq_max", 1, 2400);
	expectNumber("a flag on the line", "-v", 1, 1);

	useLine("");
	expectNumber("an empty line", "touchscreen", 0, 0);

	if (failures != 0) {
		printf("bootargs-defaults-test: %d FAILED\n", failures);
		return 1;
	}
	printf("bootargs-defaults-test: all passed\n");
	return 0;
}
