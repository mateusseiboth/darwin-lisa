/*
 * darwin-lisa: the flashlight of the Xiaomi 11 Lite 5G NE (lisa): the flash LED module of the PM8350C (SPMI slave 2,
 * registers 0xee00, PPID 0x2ee; "qcom,pm8350c-flash-led", four channels), the rear LED on its channel 0.
 *
 * Port of HakiOS's drivers/camera/hakios.torch.c (redwood) onto the shared SPMI code (sys/dlisa.spmi.h). lisa's stock
 * overlay (dtbo entry "lisa based ... SM7325") has the same module as redwood's: torch_0 / switch_0 with qcom,led-mask
 * 0x01 for every rear camera's flash ("qcom,camera-flash" torch-source pm8350c_torch0), qcom,ires-ua 12500. Register
 * layout: Linux drivers/leds/flash/leds-qcom-flash.c, mvflash_4ch_regs (checked against v7.2.9):
 *
 *   0x06 STATUS1, 0x07 STATUS2 (OTST1 0x20, OTST2 0x10, VPH_LOW 0x01), 0x10 INT_RT_STS (fault 0x01)
 *   0x3e + ch  CHAN_TIMER ([7] the safety timer)       0x42 + ch  ITARGET ([6:0] (code + 1) x resolution)
 *   0x46       MODULE_EN [7]                           0x49       IRESOLUTION (a bit per channel: 0 = 12.5 mA)
 *   0x4a + ch  CHAN_STROBE ([2] hardware strobe)       0x4e       CHAN_EN (a bit per channel)
 *
 * The torch current is Android's on redwood, 125 mA (ITARGET 9 in 12.5 mA steps); lisa's stock tree allows torch_0
 * up to 500 mA, so 125 mA is a quarter of the most the module is set up for there.
 *
 * Writing to a PMIC: every write goes through put(), which takes only the six registers of channel 0 above, only on
 * the module the arbiter's channel was checked to be, and reads every write back; any surprise ends all writing for
 * the run (the "off" writes excepted: a lit LED always gets them). The arbiter is shared: one transfer at a time
 * (dlisa_spmi_* take hakios_spmi_lock()). Writes are made only if the arbiter says this system owns the channel.
 *
 * Boot-args (nothing happens without one):
 *   torch=1        a look, reading only: the channel and its owner, the module's type, its registers
 *   torch=2        ... and the light on once, at torch_secs (default 2), then off; every write read back
 *   torch=3        the light for Darwin until the run ends: /dev/hakios-torch takes a 32-bit level 0..1000
 *                  (thousandths of 125 mA, 0 off) and a read gives what is lit; also sysctl kern.hakios.torch_level
 *                  (RW, the same level: for test plans and for libdlisa's flashlight hook)
 *   torch_max_s=N  on for N s at a time at most (default 120, 1..600), then off until 0 is asked for again
 *   torch_seq=L@S,...  test levels, each S seconds after the driver starts (torch=3 only): torch_seq=1000@200,0@205
 *
 * Guards while lit, ten times a second: the module's fault and over-temperature flags, the PM8350C's temperature
 * alarm (PPID 0x20a), the battery's temperature (45 C, hakios_battery_temperature() when a battery driver knows it).
 * Any of them, a failed read or a write that does not read back: off, and off for the rest of the run.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/dlisa.spmi.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern kern_return_t thread_terminate(thread_t thread);
extern int hakios_battery_temperature(void);

#define FLASH_PPID              0x2ee
#define ALARM_PPID              0x20a

#define FLASH_REVISION1         0x00
#define FLASH_TYPE              0x04
#define FLASH_STATUS1           0x06
#define FLASH_STATUS2           0x07
#define  STATUS2_OTST1          0x20
#define  STATUS2_OTST2          0x10
#define  STATUS2_VPH_LOW        0x01
#define FLASH_INT_RT_STS        0x10
#define  RT_STS_FAULT           0x01
#define FLASH_SAFETY_TIMER0     0x3e
#define FLASH_ITARGET0          0x42
#define FLASH_MODULE_EN         0x46
#define FLASH_IRESOLUTION       0x49
#define FLASH_STROBE_CTRL0      0x4a
#define FLASH_EN_LED_CTRL       0x4e
#define FLASH_TYPE_VAL          0x18
#define FLASH_SUBTYPE_4CH       0x07

#define ALARM_SUBTYPE           0x05
#define ALARM_STATUS            0x08

#define TORCH_MA                125
#define IRES_UA                 12500
#define MAX_ITARGET             9               /* round(125000 / 12500) - 1 */
#define HOTTEST_BATTERY_C       45

static struct dlisa_spmi_peripheral flash, alarm;
static bool alarm_found, broken, forcing, configured, lit;
static uint8_t alarm_subtype;
static uint32_t level, max_secs = 120;
static volatile uint32_t wanted, shown;
static uint32_t times_lit, times_cut;

static const uint8_t writable_regs[] = {
	FLASH_SAFETY_TIMER0, FLASH_ITARGET0, FLASH_MODULE_EN, FLASH_IRESOLUTION, FLASH_STROBE_CTRL0, FLASH_EN_LED_CTRL,
};

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
give_up(const char *why)
{
	if (broken) {
		return;
	}
	broken = true;
	printf("torch: %s: no more writing to the flash module for the rest of the run\n", why);
}

static bool
get(const struct dlisa_spmi_peripheral *p, uint8_t reg, uint8_t *value)
{
	const char *problem = dlisa_spmi_read(p, reg, value, 1);

	if (problem != NULL) {
		printf("torch: reading %#04x of PPID %#x: %s\n", reg, p->ppid, problem);
	}
	return problem == NULL;
}

static bool
is_writable_reg(uint8_t reg)
{
	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		if (writable_regs[i] == reg) {
			return true;
		}
	}
	return false;
}

/* one register of the six, written and read back */
static bool
put(uint8_t reg, uint8_t value)
{
	uint8_t back = 0xff;
	const char *problem;

	if ((broken && !forcing) || !is_writable_reg(reg) || !flash.writable || flash.ppid != FLASH_PPID) {
		give_up("a write that may not be made");
		return false;
	}
	problem = dlisa_spmi_write(&flash, reg, &value, 1);
	problem = problem != NULL ? problem : dlisa_spmi_read(&flash, reg, &back, 1);
	if (problem != NULL || back != value) {
		printf("torch: %#04x <- %#04x: %s (reads %#04x)\n", reg, value, problem != NULL ? problem : "not read back", back);
		give_up("a write that failed or did not read back");
		return false;
	}
	return true;
}

static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(&flash, reg, &now)) {
		give_up("a read that failed");
		return false;
	}
	return (now & mask) == (value & mask) || put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

static void
show_registers(void)
{
	uint8_t r[8];

	for (unsigned base = 0; base < 0x100; base += 0x10) {
		bool ok = dlisa_spmi_read(&flash, (uint8_t)base, r, 8) == NULL;
		uint8_t s[8] = { 0 };

		ok = ok && dlisa_spmi_read(&flash, (uint8_t)(base + 8), s, 8) == NULL;
		ok ? (void)printf("torch: %#04x: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    base, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]) :
		    (void)printf("torch: %#04x: (unreadable)\n", base);
	}
}

static uint8_t
alarm_stage(uint8_t status)
{
	return alarm_subtype == 0x08 ? status & 0x03 : (status >> 4) & 0x07;
}

/* what would make the light go off now, or NULL */
static const char *
trouble(void)
{
	static char why[160];
	uint8_t rt = 0, status1 = 0, status2 = 0, alarm_status = 0;

	if (!get(&flash, FLASH_INT_RT_STS, &rt) || !get(&flash, FLASH_STATUS1, &status1) ||
	    !get(&flash, FLASH_STATUS2, &status2) || (alarm_found && !get(&alarm, ALARM_STATUS, &alarm_status))) {
		return "a read of the flags that failed";
	}
	if ((rt & RT_STS_FAULT) || status1 != 0 || (status2 & (STATUS2_OTST1 | STATUS2_OTST2 | STATUS2_VPH_LOW)) ||
	    alarm_stage(alarm_status) != 0) {
		snprintf(why, sizeof(why), "the module says fault %#x, channels %#x, temperature and supply %#x; the PMIC's "
		    "temperature stage %u", rt, status1, status2, alarm_stage(alarm_status));
		return why;
	}
	if (hakios_battery_temperature() >= HOTTEST_BATTERY_C) {
		snprintf(why, sizeof(why), "the battery is at %d C", hakios_battery_temperature());
		return why;
	}
	return NULL;
}

/* Android's probe, once: channel 0 strobed by software, its safety timer off */
static bool
configure(void)
{
	configured = configured || (put_bits(FLASH_STROBE_CTRL0, 0x74, 0x00) && put_bits(FLASH_SAFETY_TIMER0, 0x80, 0x00));
	return configured;
}

static uint8_t
itarget_for(uint32_t permille)
{
	uint32_t ua = TORCH_MA * permille, code;

	ua = ua < IRES_UA ? IRES_UA : ua;
	code = (ua + IRES_UA / 2) / IRES_UA - 1;
	return (uint8_t)(code > MAX_ITARGET ? MAX_ITARGET : code);
}

static bool
light(uint32_t permille)
{
	const char *why = lit ? NULL : trouble();

	if (why != NULL) {
		printf("torch: not lit: %s\n", why);
		give_up("trouble before the light");
		return false;
	}
	if (!configure() || !put_bits(FLASH_IRESOLUTION, 0x01, 0x00) || !put_bits(FLASH_ITARGET0, 0x7f, itarget_for(permille))) {
		return false;
	}
	if (lit) {
		return true;
	}
	lit = true;             /* before the enabling writes: one that lands but fails its read-back still gets dark() */
	return put(FLASH_MODULE_EN, 0x80) && put_bits(FLASH_EN_LED_CTRL, 0x01, 0x01);
}

/* off, in Android's order; every step tried, none stopped by `broken' */
static bool
dark(void)
{
	bool ok;

	forcing = true;
	ok = put_bits(FLASH_EN_LED_CTRL, 0x01, 0x00);
	ok = put(FLASH_MODULE_EN, 0x00) && ok;
	ok = put_bits(FLASH_ITARGET0, 0x7f, 0x00) && ok;
	forcing = false;
	lit = lit && !ok;
	return ok;
}

/* ---- the light for Darwin (torch=3): /dev/hakios-torch, kern.hakios.torch_level ---------------------------- */

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = 0;
	int error = 0;

	while (error == 0 && uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		error = uiomove((caddr_t)&value, sizeof(value), uio);
		wanted = error == 0 ? (value > 1000 ? 1000 : value) : wanted;
	}
	return error;
}

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

SYSCTL_DECL(_kern_hakios);

static int
sysctl_torch_level SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = (int)shown, error = SYSCTL_OUT(req, &value, sizeof(value));

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	error = SYSCTL_IN(req, &value, sizeof(value));
	if (error != 0 || level < 3) {
		return error != 0 ? error : ENODEV;
	}
	wanted = value < 0 ? 0 : value > 1000 ? 1000 : (uint32_t)value;
	return 0;
}

static int
sysctl_torch_status SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[120];

	snprintf(text, sizeof(text), "level %u lit %u wanted %u times %u cut %u broken %u", level, shown, wanted, times_lit,
	    times_cut, broken);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, torch_level, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    sysctl_torch_level, "I", "the flashlight: 0 off .. 1000 (125 mA); reads what is lit (torch=3)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, torch_status, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctl_torch_status, "A", "the flashlight driver's state");

static bool
apply(uint32_t want, uint32_t applied)
{
	bool ok = want == 0 ? dark() : light(want);

	if (ok) {
		times_lit += applied == 0 && want != 0;
		shown = want;
		printf("torch: %s (level %u of 1000; battery %d C)\n", want ? "ON" : "off", want, hakios_battery_temperature());
	}
	return ok;
}

static void
serve(void)
{
	uint32_t applied = 0;
	uint64_t since = 0, last_look = 0;
	bool waiting_for_off = false;
	int major = cdevsw_add(-1, &torch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
		printf("torch: /dev/hakios-torch cannot be made\n");
		return;
	}
	printf("torch: the light is Darwin's (/dev/hakios-torch, kern.hakios.torch_level): at most %u mA, at most %u s at a "
	    "time\n", TORCH_MA, max_secs);
	while (!broken) {
		uint32_t want = wanted;
		uint64_t now = now_ms();
		const char *why;

		waiting_for_off = waiting_for_off && want != 0;
		want = waiting_for_off ? 0 : want;
		if (want != applied && !apply(want, applied)) {
			break;
		}
		since = want != applied && applied == 0 ? now : since;
		applied = want;
		if (applied != 0 && now - since >= max_secs * 1000ull) {
			times_cut++;
			printf("torch: on for %u s, the limit: off until Darwin asks for 0 and then again\n", max_secs);
			waiting_for_off = true;
			continue;
		}
		why = applied != 0 && now - last_look >= 100 ? trouble() : NULL;
		last_look = applied != 0 && now - last_look >= 100 ? now : last_look;
		if (why != NULL) {
			printf("torch: OFF: %s\n", why);
			(void)dark();
			give_up("trouble while lit");
			break;
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	lit ? (void)dark() : (void)0;
	shown = 0;
	printf("torch: the light is given up on (%u times lit, %u cut)\n", times_lit, times_cut);
}

static void
light_once(void)
{
	uint32_t secs = 2;
	uint64_t until;
	const char *why = NULL;

	PE_parse_boot_argn("torch_secs", &secs, sizeof(secs));
	secs = secs < 1 ? 1 : secs > max_secs ? max_secs : secs;
	printf("torch: the light ON at %u mA for %u s\n", TORCH_MA, secs);
	if (!light(1000)) {
		lit ? (void)dark() : (void)0;
		return;
	}
	delay_for_interval(50, NSEC_PER_MSEC);
	show_registers();
	for (until = now_ms() + secs * 1000ull; now_ms() < until && (why = trouble()) == NULL;) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	why != NULL ? (void)printf("torch: OFF early: %s\n", why) : (void)0;
	printf("torch: off again: %s\n", dark() ? "done" : "FAILED");
	show_registers();
}

static bool
look(void)
{
	uint8_t id[2] = { 0, 0 }, revision = 0, alarm_status = 0;
	const char *problem = dlisa_spmi_find(FLASH_PPID, &flash);

	if (problem != NULL) {
		printf("torch: the flash module (PPID %#x): %s\n", FLASH_PPID, problem);
		return false;
	}
	alarm_found = dlisa_spmi_find(ALARM_PPID, &alarm) == NULL && get(&alarm, ALARM_SUBTYPE, &alarm_subtype) &&
	    get(&alarm, ALARM_STATUS, &alarm_status);
	if (dlisa_spmi_read(&flash, FLASH_TYPE, id, 2) != NULL || dlisa_spmi_read(&flash, FLASH_REVISION1, &revision, 1) != NULL) {
		printf("torch: the flash module cannot be read\n");
		return false;
	}
	printf("torch: channel %u (%s); type %#x, subtype %#x, revision %#x%s; temperature alarm %s, stage %u\n",
	    flash.channel, flash.writable ? "this system's" : "NOT this system's", id[0], id[1], revision,
	    id[0] == FLASH_TYPE_VAL && id[1] == FLASH_SUBTYPE_4CH ? " (the 4-channel module)" : " (NOT WHAT WAS EXPECTED)",
	    alarm_found ? "read" : "not found", alarm_found ? alarm_stage(alarm_status) : 0);
	show_registers();
	return id[0] == FLASH_TYPE_VAL && id[1] == FLASH_SUBTYPE_4CH && flash.writable;
}

#define SEQ_MAX 16
static char seq_text[160];

static uint64_t
parse_decimal(const char *text)
{
	uint64_t value = 0;

	for (; *text >= '0' && *text <= '9'; text++) {
		value = value * 10 + (uint64_t)(*text - '0');
	}
	return value;
}

/* torch_seq=1000@200,0@205: levels for test plans, as Darwin's would come */
static void
seq_thread(__unused void *parameter, __unused wait_result_t result)
{
	const char *cursor = seq_text;
	uint64_t start = now_ms();

	for (int n = 0; n < SEQ_MAX && *cursor != 0; n++) {
		const char *at = strchr(cursor, '@'), *end = strchr(cursor, ',');
		uint64_t value = parse_decimal(cursor), when = at != NULL ? parse_decimal(at + 1) * 1000 : 0;

		if (at == NULL) {
			break;
		}
		while (now_ms() < start + when) {
			delay_for_interval(100, NSEC_PER_MSEC);
		}
		printf("torch: torch_seq: level %llu asked for\n", value);
		wanted = value > 1000 ? 1000 : (uint32_t)value;
		cursor = end != NULL ? end + 1 : "";
	}
	thread_terminate(current_thread());
}

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	thread_t thread;
	bool can_write = look();

	PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
	max_secs = max_secs < 1 ? 1 : max_secs > 600 ? 600 : max_secs;
	if (can_write && level == 2) {
		light_once();
	}
	if (can_write && level == 3 && PE_parse_boot_arg_str("torch_seq", seq_text, sizeof(seq_text)) &&
	    kernel_thread_start(seq_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (can_write && level == 3) {
		serve();
	}
	printf("torch: done\n");
	thread_terminate(current_thread());
}

void dlisa_torch_init(void);

void
dlisa_torch_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("torch", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 3 ? 3 : level;
	printf("torch: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the light on once" :
	    "the light for Darwin");
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
