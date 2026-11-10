/*
 * darwin-lisa: the real-time clock of lisa (Xiaomi 11 Lite 5G NE, SM7325): the PMK8350's RTC, over SPMI.
 *
 * The kernel asks the platform for the time once, at boot (clock_initialize_calendar -> PEGetUTCTimeOfDay), and tells
 * it whenever the system sets the time (settimeofday -> clock_set_calendar_microtime -> PESetUTCTimeOfDay). There is no
 * platform clock on this port, so both answered nothing until now and the calendar started at 1970 (timed then put it
 * at the build's time). rtc-wire.py makes the two ask this file: dlisa_rtc_get() and dlisa_rtc_set().
 *
 * The clock: "qcom,pmk8350-rtc" at rtc@6100 of the PMK8350 (slave 0) in lisa's device tree, registers as Linux's
 * drivers/rtc/rtc-pm8xxx.c has them for that part (pmk8350_regs): RTC_WRITE 0x6140..0x6143, RTC_CTRL 0x6146 (bit 7:
 * the counter runs), RTC_READ 0x6148..0x614b, a little-endian count of seconds. A read is taken again when its low byte
 * rolled over between the first and the second look, as Linux does.
 *
 * The count is not the time. Like every Qualcomm phone, the PMIC's counter runs from whatever it was when the battery
 * was connected, and nobody on this phone's side may usually write it (Linux on lisa, pmOS, has neither
 * "allow-set-time" nor an nvmem "offset" cell: its rtc0 is the bare count). The time is the count plus an OFFSET, as
 * Android's time_daemon keeps it. Where the offset comes from, best first:
 *
 *   boot-arg rtc_offset=S     a host run that knows it (tools/rtc-offset.sh measures it from pmOS's rtc0)
 *   init26's saved one        /private/var/db/dlisa-rtc-offset on the data volume, written to kern.hakios.rtc_offset
 *                             early in the boot (userland/init/rtc-offset.c); refused while a boot-arg rules
 *   the system's own          every settimeofday (timed's network time, Settings' "Set Automatically" off) gives the
 *                             offset of that moment; init26's keeper saves it, so the next boot starts from it
 *   none                      the count itself when it reads like a date after 2024 (the clock was set by somebody),
 *                             else boot-arg epoch= (the build's time: wrong by the time since the build, no worse than
 *                             before), else the count (a date in the 1970s: timed lifts it to the build's time)
 *
 * Boot-args:
 *   rtc=1           on (off by default: then both calls answer nothing, as before)
 *   rtc_offset=S    the offset, seconds (signed)
 *   rtc_write=1     a time set by the system is also written into the PMIC's counter, when the arbiter says the clock's
 *                   channel is this system's; the offset is 0 then. Off by default: the counter is left as it was.
 *
 * Sysctls (kern.hakios.): rtc_count (the counter now), rtc_offset (read; written by init26), rtc_source (where the
 * offset came from: 0 none, 1 the count is a date, 2 epoch=, 3 rtc_offset=, 4 init26's saved one, 5 set by the system).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/dlisa.spmi.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

/* the platform's clock, asked by iokit/Kernel/IOPlatformExpert.cpp (rtc-wire.py) */
bool dlisa_rtc_get(clock_sec_t *secs, clock_nsec_t *nsecs);
void dlisa_rtc_set(clock_sec_t secs);

#define RTC_PPID                0x061           /* slave 0 (PMK8350), peripheral 0x61: registers 0x6100 */
#define RTC_TYPE                0x04            /* peripheral type, subtype */
#define RTC_WRITE               0x40            /* 4 bytes, little-endian */
#define RTC_CTRL                0x46
#define RTC_CTRL_ENABLE         0x80
#define RTC_READ                0x48            /* 4 bytes, little-endian */

#define DATE_2024               1704067200LL    /* 2024-01-01 00:00:00 UTC: a count above it was set to the time */

enum {
	RTC_SOURCE_NONE,                        /* the bare count, or nothing at all */
	RTC_SOURCE_COUNT,                       /* the count reads like a date: taken as UTC */
	RTC_SOURCE_EPOCH,                       /* estimated from boot-arg epoch= (the build's time) */
	RTC_SOURCE_BOOTARG,                     /* boot-arg rtc_offset= */
	RTC_SOURCE_SAVED,                       /* init26, from the data volume */
	RTC_SOURCE_SET,                         /* the system set the time */
};

static const char *const source_names[] = {
	"none: the counter's own count", "the counter reads like a date (taken as UTC)", "boot-arg epoch= (the build's time)",
	"boot-arg rtc_offset=", "init26's saved offset", "the time the system set",
};

static LCK_GRP_DECLARE(rtc_lock_group, "dlisa-rtc");
static LCK_MTX_DECLARE(rtc_lock, &rtc_lock_group);
static bool rtc_looked;                         /* the boot-args were read and the clock looked for, once */
static bool rtc_on;                             /* rtc=1 and the clock answers */
static bool rtc_write_back;                     /* rtc_write=1 */
static struct dlisa_spmi_peripheral rtc;
static int64_t rtc_offset;
static int rtc_source = RTC_SOURCE_NONE;
static unsigned sets_said;

/* "YYYY-MM-DD hh:mm:ss UTC" of seconds since 1970 (civil from days, H. Hinnant's algorithm) */
static void
format_time(int64_t secs, char *out, size_t size)
{
	int64_t days = secs / 86400, rest = secs % 86400, era, year;
	unsigned day_of_era, year_of_era, day_of_year, shifted_month, day, month;

	if (rest < 0) {
		rest += 86400;
		days--;
	}
	days += 719468;
	era = (days >= 0 ? days : days - 146096) / 146097;
	day_of_era = (unsigned)(days - era * 146097);
	year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	shifted_month = (5 * day_of_year + 2) / 153;
	day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
	month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
	year = (int64_t)year_of_era + era * 400 + (month <= 2);
	snprintf(out, size, "%04lld-%02u-%02u %02lld:%02lld:%02lld UTC", year, month, day, rest / 3600, rest / 60 % 60, rest % 60);
}

/* The counter, in seconds. NULL, or what went wrong. */
static const char *
read_count(uint32_t *count)
{
	uint8_t value[4], again;
	const char *problem;

	if ((problem = dlisa_spmi_read(&rtc, RTC_READ, value, 4)) != NULL ||
	    (problem = dlisa_spmi_read(&rtc, RTC_READ, &again, 1)) != NULL) {
		return problem;
	}
	/* the low byte rolled over between the two looks: the upper ones may be the second's before */
	if (again < value[0] && (problem = dlisa_spmi_read(&rtc, RTC_READ, value, 4)) != NULL) {
		return problem;
	}
	*count = (uint32_t)value[0] | (uint32_t)value[1] << 8 | (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
	return NULL;
}

/* rtc_write=1: the counter set to `secs`, stopped around the write as Linux's __pm8xxx_rtc_set_time() does */
static const char *
write_count(uint32_t secs)
{
	uint8_t value[4] = { (uint8_t)secs, (uint8_t)(secs >> 8), (uint8_t)(secs >> 16), (uint8_t)(secs >> 24) };
	uint8_t control, stopped, running, zero = 0;
	const char *problem, *restart;

	if ((problem = dlisa_spmi_read(&rtc, RTC_CTRL, &control, 1)) != NULL) {
		return problem;
	}
	stopped = (uint8_t)(control & ~RTC_CTRL_ENABLE);
	running = (uint8_t)(control | RTC_CTRL_ENABLE);
	if ((problem = dlisa_spmi_write(&rtc, RTC_CTRL, &stopped, 1)) != NULL) {
		return problem;
	}
	/* byte 0 first to 0, so that no carry runs into the bytes being written, then 1..3, then byte 0 */
	if ((problem = dlisa_spmi_write(&rtc, RTC_WRITE, &zero, 1)) == NULL &&
	    (problem = dlisa_spmi_write(&rtc, RTC_WRITE + 1, value + 1, 3)) == NULL) {
		problem = dlisa_spmi_write(&rtc, RTC_WRITE, value, 1);
	}
	restart = dlisa_spmi_write(&rtc, RTC_CTRL, &running, 1);
	return problem != NULL ? problem : restart;
}

/* The offset there is before anybody tells one, for this count. */
static void
choose_offset(uint32_t count)
{
	int64_t given = 0;
	uint64_t epoch = 0;

	if (PE_parse_boot_argn("rtc_offset", &given, sizeof(given))) {
		rtc_offset = given;
		rtc_source = RTC_SOURCE_BOOTARG;
	} else if ((int64_t)count >= DATE_2024) {
		rtc_offset = 0;
		rtc_source = RTC_SOURCE_COUNT;
	} else if (PE_parse_boot_argn("epoch", &epoch, sizeof(epoch)) && (int64_t)epoch >= DATE_2024) {
		rtc_offset = (int64_t)epoch - (int64_t)count;
		rtc_source = RTC_SOURCE_EPOCH;
	} else {
		rtc_offset = 0;
		rtc_source = RTC_SOURCE_NONE;
	}
}

/* Once: rtc=1, the clock's channel, its state, and the first offset. Called with rtc_lock held. */
static void
look_once(void)
{
	uint32_t enabled = 0, write_back = 0, count = 0;
	uint8_t type[2] = { 0 }, control = 0;
	const char *problem;
	char when[40];

	if (rtc_looked) {
		return;
	}
	rtc_looked = true;
	if (!PE_parse_boot_argn("rtc", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	PE_parse_boot_argn("rtc_write", &write_back, sizeof(write_back));
	rtc_write_back = write_back != 0;
	if ((problem = dlisa_spmi_find(RTC_PPID, &rtc)) != NULL) {
		printf("rtc: the PMK8350's clock (slave 0, peripheral 0x61): %s; no clock\n", problem);
		return;
	}
	if ((problem = dlisa_spmi_read(&rtc, RTC_TYPE, type, 2)) != NULL ||
	    (problem = dlisa_spmi_read(&rtc, RTC_CTRL, &control, 1)) != NULL ||
	    (problem = read_count(&count)) != NULL) {
		printf("rtc: the PMK8350's clock (channel %u): %s; no clock\n", rtc.channel, problem);
		return;
	}
	printf("rtc: the PMK8350's clock: channel %u (%s), type %#x subtype %#x, control %#x (%s), count %u s\n", rtc.channel,
	    rtc.writable ? "this system may write it" : "read only for this system", type[0], type[1], control,
	    control & RTC_CTRL_ENABLE ? "running" : "STOPPED", count);
	if (!(control & RTC_CTRL_ENABLE)) {
		uint8_t running = (uint8_t)(control | RTC_CTRL_ENABLE);

		problem = rtc_write_back ? dlisa_spmi_write(&rtc, RTC_CTRL, &running, 1) : "rtc_write=1 would start it";
		printf("rtc: the counter does not run: %s\n", problem != NULL ? problem : "started");
	}
	rtc_on = true;
	choose_offset(count);
	format_time((int64_t)count + rtc_offset, when, sizeof(when));
	printf("rtc: offset %lld s from %s: the time is %s\n", rtc_offset, source_names[rtc_source], when);
}

bool
dlisa_rtc_get(clock_sec_t *secs, clock_nsec_t *nsecs)
{
	uint32_t count;
	int64_t now;
	const char *problem = NULL;

	lck_mtx_lock(&rtc_lock);
	look_once();
	if (!rtc_on || (problem = read_count(&count)) != NULL) {
		lck_mtx_unlock(&rtc_lock);
		if (problem != NULL) {
			printf("rtc: the clock is not read: %s\n", problem);
		}
		return false;
	}
	now = (int64_t)count + rtc_offset;
	lck_mtx_unlock(&rtc_lock);
	if (now <= 0) {
		return false;
	}
	*secs = (clock_sec_t)now;
	*nsecs = 0;
	return true;
}

void
dlisa_rtc_set(clock_sec_t secs)
{
	uint32_t count;
	int64_t was, offset;
	const char *problem = NULL, *written = "not asked (rtc_write=0)";
	bool changed = false;
	char when[40];

	lck_mtx_lock(&rtc_lock);
	look_once();
	if (!rtc_on || (problem = read_count(&count)) != NULL) {
		lck_mtx_unlock(&rtc_lock);
		return;
	}
	was = rtc_offset;
	offset = (int64_t)secs - (int64_t)count;
	if (rtc_write_back && secs <= UINT32_MAX && (written = write_count((uint32_t)secs)) == NULL) {
		offset = 0;
	}
	/*
	 * A count has a second's grain: an offset within a second of the one there is is the same (init26's own setting
	 * of the saved one comes back here that way). An estimate gives way to any time set.
	 */
	if (offset - was > 1 || was - offset > 1 || rtc_source < RTC_SOURCE_BOOTARG) {
		rtc_offset = offset;
		rtc_source = RTC_SOURCE_SET;
		changed = true;
	}
	lck_mtx_unlock(&rtc_lock);
	if (changed && sets_said < 20) {
		sets_said++;
		format_time((int64_t)secs, when, sizeof(when));
		printf("rtc: the system set the time to %s: offset %lld s (was %lld); the counter written: %s\n", when, offset,
		    was, written == NULL ? "yes" : written);
	}
}

/* init26's saved offset (sysctl kern.hakios.rtc_offset): the calendar goes to count + offset at once */
static int
apply_saved_offset(int64_t offset)
{
	uint32_t count;
	int64_t now;
	char when[40];

	lck_mtx_lock(&rtc_lock);
	look_once();
	if (!rtc_on || read_count(&count) != NULL) {
		lck_mtx_unlock(&rtc_lock);
		return ENXIO;
	}
	if (rtc_source == RTC_SOURCE_BOOTARG) {
		lck_mtx_unlock(&rtc_lock);
		printf("rtc: init26's saved offset %lld s is not used: boot-arg rtc_offset= rules this boot\n", offset);
		return EBUSY;
	}
	now = (int64_t)count + offset;
	if (now < DATE_2024) {
		lck_mtx_unlock(&rtc_lock);
		printf("rtc: init26's saved offset %lld s makes a date before 2024: not used\n", offset);
		return EINVAL;
	}
	rtc_offset = offset;
	rtc_source = RTC_SOURCE_SAVED;
	lck_mtx_unlock(&rtc_lock);
	/* (it comes back through dlisa_rtc_set(), within a second of this offset: kept as it is) */
	clock_set_calendar_microtime((clock_sec_t)now, 0);
	format_time(now, when, sizeof(when));
	printf("rtc: offset %lld s from %s: the time is %s\n", offset, source_names[RTC_SOURCE_SAVED], when);
	return 0;
}

SYSCTL_DECL(_kern_hakios);

static int
sysctl_rtc_count SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int64_t value = -1;
	uint32_t count;

	lck_mtx_lock(&rtc_lock);
	look_once();
	if (rtc_on && read_count(&count) == NULL) {
		value = count;
	}
	lck_mtx_unlock(&rtc_lock);
	return SYSCTL_OUT(req, &value, sizeof(value));
}

static int
sysctl_rtc_offset SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int64_t value;
	int error;

	lck_mtx_lock(&rtc_lock);
	value = rtc_offset;
	lck_mtx_unlock(&rtc_lock);
	error = SYSCTL_OUT(req, &value, sizeof(value));
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (req->newlen != sizeof(value)) {
		return EINVAL;
	}
	if ((error = SYSCTL_IN(req, &value, sizeof(value))) != 0) {
		return error;
	}
	return apply_saved_offset(value);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, rtc_count, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_rtc_count, "Q",
    "the PMK8350 clock's counter now, in seconds, or -1");
SYSCTL_PROC(_kern_hakios, OID_AUTO, rtc_offset, CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_rtc_offset, "Q",
    "seconds to add to the counter for UTC; a write applies it at once (init26)");
SYSCTL_INT(_kern_hakios, OID_AUTO, rtc_source, CTLFLAG_RD | CTLFLAG_LOCKED, &rtc_source, 0,
    "where the offset came from: 0 none, 1 the count is a date, 2 epoch=, 3 rtc_offset=, 4 saved, 5 set by the system");
