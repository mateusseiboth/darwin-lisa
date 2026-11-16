/*
 * darwin-lisa: the three keys of the Xiaomi 11 Lite 5G NE (lisa) as button records of /dev/hakios-touch
 * (sys/hakios.input.h). libdlisa inside backboardd turns them into HID consumer events of its "darwin-lisa buttons"
 * service (userland/inject/dlisa-inject.c, sendButton()): Power (0x0C/0x30) is the side button, 0xE9/0xEA the volume
 * keys. backboardd does the rest (lock, wake, volume HUD, side + volume up for a screenshot, long press for Siri / the
 * power-off slider).
 *
 * Port of HakiOS's drivers/input/hakios.buttons.c (redwood, proven by hand on that phone), onto the shared SPMI
 * code of this port (sys/dlisa.spmi.h). lisa has the same three inputs at the same addresses; both device trees say so
 * (pmOS's sm7325-xiaomi-lisa.dtb: pmk8350 pon@1300 pwrkey/resin, gpio-keys on pm7325_gpios gpio6; the stock lisa
 * overlay: the same):
 *
 *   power        KPDPWR_N of the PMK8350's power-on block, SPMI slave 0, registers 0x1300 (PPID 0x013)
 *   volume down  RESIN_N of the same block
 *   volume up    GPIO6 of the PM7325, slave 1, registers 0x8d00 (PPID 0x18d), pulled up; the key pulls it low
 *
 * THIS DRIVER ONLY READS the PMICs. The firmware reads the same keys (volume up / down choose recovery or fastboot)
 * and leaves the inputs set up; a key whose input is not what it should be is left alone, and said so.
 *
 * A thread reads the PON's real-time status (a bit per key, debounced by the block) and the GPIO's level every
 * POLL_MS; a key has changed when two looks in a row agree. A key that reads as held before it has been seen
 * released is not believed.
 *
 * Boot-args:
 *   buttons=1              on (default: on when touchscreen= is on, which lisa=1 gives); buttons=0 off
 *   buttons_steps=N        stop after step N (1 arbiter, 2 channels, 3 identify, 4 inputs, 5 watch: the default)
 *   buttons_seq=K@S,...    test presses, given to Darwin exactly as the keys' own (no PMIC involved), each S seconds
 *                          after the driver starts. K: pw (power, 150 ms), pwlong (power, 3.5 s), vu, vd (volume, 150 ms),
 *                          shot (power and volume up together: the screenshot), wake (power, 150 ms; same as pw)
 *   press_power=S          HakiOS's old test: power pressed at S s and again at S+10 s (press_power_ms=, default 150)
 *
 * Sysctls (read back by the test plans):
 *   kern.hakios.buttons_state    bit 0 power, 1 volume up, 2 volume down: held now (as reported)
 *   kern.hakios.buttons_presses  "power N volume-up N volume-down N injected N reads N failed N"
 *   kern.hakios.buttons_inject   write K (1 power, 2 volume up, 3 volume down) * 1 for a tap, + 100 for a 3.5 s press:
 *                                a press posted now, as the test sequence posts them
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/dlisa.spmi.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);

#define PON_PPID                0x013   /* PMK8350 (slave 0), power-on block for the OS ("HLOS"), 0x1300 */
#define VOLUP_PPID              0x18d   /* PM7325 (slave 1), GPIO6: 0x8800 + (6 - 1) * 0x100 */

#define REG_TYPE                0x04
#define REG_RT_STATUS           0x10
#define REG_INT_SET_TYPE        0x11
#define REG_INT_POLARITY_HIGH   0x12
#define REG_INT_POLARITY_LOW    0x13
#define REG_INT_ENABLED         0x15

#define PON_TYPE                0x01
#define PON_SUBTYPE_HLOS        0x09    /* gen 3 PON, the OS's half (Linux pm8941-pwrkey "pmk8350") */
#define PON_KPDPWR              0x80
#define PON_RESIN               0x40

#define GPIO_TYPE               0x10
#define GPIO_SUBTYPE_FIRST      0x10    /* LV .. MV_VIN3: one register layout (Linux pinctrl-spmi-gpio.c) */
#define GPIO_SUBTYPE_LAST       0x13
#define GPIO_LEVEL              0x01
#define GPIO_REG_MODE           0x40
#define GPIO_REG_PULL           0x42
#define GPIO_REG_ENABLE         0x46

#define POLL_MS                 16
#define MINUTE_TICKS            (60 * 1000 / POLL_MS)
#define TAP_MS                  150
#define LONG_MS                 3500

static struct dlisa_spmi_peripheral pon, volup_pin;

static struct button {
	const char *name;
	uint32_t record;                /* HAKIOS_BUTTON_* */
	bool usable, live, last, down;
	uint64_t since;
	uint32_t presses;
} power = { .name = "power", .record = HAKIOS_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = HAKIOS_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = HAKIOS_BUTTON_VOLUME_DOWN };

static struct {
	uint64_t reads, failed;
} bus;
static uint32_t injected;
static int buttons_state;

static void
update_state(void)
{
	buttons_state = (power.down ? 1 : 0) | (volume_up.down ? 2 : 0) | (volume_down.down ? 4 : 0);
}

static uint64_t
uptime_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static const char *
read_reg(const struct dlisa_spmi_peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = dlisa_spmi_read(p, reg, bytes, count);

	bus.reads++;
	bus.failed += problem != NULL;
	return problem;
}

static void
button_look(struct button *b, bool pressed)
{
	bool steady = pressed == b->last;
	uint64_t now = uptime_ms();

	b->last = pressed;
	if (!b->usable || !steady) {
		return;
	}
	if (!b->live) {
		b->live = !pressed;
		if (b->live) {
			printf("buttons: %s is released now: believed from here on\n", b->name);
		}
		return;
	}
	if (pressed == b->down) {
		return;
	}
	b->down = pressed;
	update_state();
	if (pressed) {
		b->since = now;
		b->presses++;
		printf("buttons: %s PRESSED at %llu.%03llu s\n", b->name, now / 1000, now % 1000);
	} else {
		printf("buttons: %s released at %llu.%03llu s, after %llu ms\n", b->name, now / 1000, now % 1000, now - b->since);
	}
	hakios_touch_post(pressed ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, b->record, 0, 0);
}

static void
poll_pon(void)
{
	static unsigned said;
	uint8_t status;
	const char *problem;

	if (!power.usable) {
		return;
	}
	problem = read_reg(&pon, REG_RT_STATUS, &status, 1);
	if (problem != NULL) {
		said < 8 ? (void)printf("buttons: reading the power-on block: %s\n", problem) : (void)0;
		said++;
		return;
	}
	button_look(&power, status & PON_KPDPWR);
	button_look(&volume_down, status & PON_RESIN);
}

static void
poll_volup(void)
{
	static unsigned said;
	uint8_t status;
	const char *problem;

	if (!volume_up.usable) {
		return;
	}
	problem = read_reg(&volup_pin, REG_RT_STATUS, &status, 1);
	if (problem != NULL) {
		said < 8 ? (void)printf("buttons: reading volume up's pin: %s\n", problem) : (void)0;
		said++;
		return;
	}
	button_look(&volume_up, !(status & GPIO_LEVEL));
}

static bool
find_peripheral(const char *name, uint16_t ppid, struct dlisa_spmi_peripheral *p)
{
	const char *problem = dlisa_spmi_find(ppid, p);

	if (problem != NULL) {
		printf("buttons: %s (PPID %#x): %s\n", name, ppid, problem);
		return false;
	}
	printf("buttons: %s is channel %u of the arbiter%s\n", name, p->channel, p->writable ? " (this system's)" : "");
	return true;
}

static bool
identify(const char *name, const struct dlisa_spmi_peripheral *p, uint8_t type, uint8_t first, uint8_t last)
{
	uint8_t is[2];
	const char *problem = read_reg(p, REG_TYPE, is, 2);

	if (problem != NULL) {
		printf("buttons: %s: %s\n", name, problem);
		return false;
	}
	printf("buttons: %s says type %#x, subtype %#x\n", name, is[0], is[1]);
	if (is[0] != type || is[1] < first || is[1] > last) {
		printf("buttons: ... not what this driver knows (type %#x, subtype %#x..%#x): left alone\n", type, first, last);
		return false;
	}
	return true;
}

static void
pon_inputs(void)
{
	uint8_t r[6];
	const char *problem = read_reg(&pon, REG_RT_STATUS, r, 6);

	if (problem != NULL) {
		printf("buttons: power-on block: %s\n", problem);
		return;
	}
	printf("buttons: power-on block: status %#x (power %s, volume down %s); interrupts: edge %#x, high %#x, low %#x, "
	    "enabled %#x\n", r[0], (r[0] & PON_KPDPWR) ? "HELD" : "released", (r[0] & PON_RESIN) ? "HELD" : "released",
	    r[REG_INT_SET_TYPE - REG_RT_STATUS], r[REG_INT_POLARITY_HIGH - REG_RT_STATUS],
	    r[REG_INT_POLARITY_LOW - REG_RT_STATUS], r[REG_INT_ENABLED - REG_RT_STATUS]);
	power.usable = volume_down.usable = true;
	power.last = r[0] & PON_KPDPWR;
	volume_down.last = r[0] & PON_RESIN;
}

static void
volup_input(void)
{
	uint8_t c[7], status = 0;
	unsigned mode, pull;
	bool enabled;
	const char *problem = read_reg(&volup_pin, GPIO_REG_MODE, c, 7);

	problem = problem != NULL ? problem : read_reg(&volup_pin, REG_RT_STATUS, &status, 1);
	if (problem != NULL) {
		printf("buttons: volume up's pin: %s\n", problem);
		return;
	}
	mode = c[0] & 3;
	pull = c[GPIO_REG_PULL - GPIO_REG_MODE] & 7;
	enabled = (c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80) != 0;
	printf("buttons: volume up's pin: mode %u, pull %u, %s, level %s\n", mode, pull, enabled ? "enabled" : "DISABLED",
	    (status & GPIO_LEVEL) ? "high (released)" : "LOW (held)");
	if (mode != 0 || pull >= 4 || !enabled) {
		printf("buttons: volume up's pin is not an enabled input with a pull-up (making it one is a PMIC write, not "
		    "done here): volume up left alone\n");
		return;
	}
	volume_up.usable = true;
	volume_up.last = !(status & GPIO_LEVEL);
}

static void
say_summary(void)
{
	printf("buttons: so far %u presses of power, %u of volume up, %u of volume down, %u injected; %llu reads, %llu "
	    "failed\n", power.presses, volume_up.presses, volume_down.presses, injected, bus.reads, bus.failed);
}

static bool
bring_up(uint32_t steps)
{
	bool pon_found, pin_found, pon_known, pin_known;

	printf("buttons: steps 1-2: the arbiter's channels of the PMK8350 power-on block and the PM7325's GPIO6\n");
	pon_found = steps >= 2 && find_peripheral("the power-on block", PON_PPID, &pon);
	pin_found = steps >= 2 && find_peripheral("volume up's pin", VOLUP_PPID, &volup_pin);
	if (steps < 3) {
		return false;
	}
	pon_known = pon_found && identify("the power-on block", &pon, PON_TYPE, PON_SUBTYPE_HLOS, PON_SUBTYPE_HLOS);
	pin_known = pin_found && identify("volume up's pin", &volup_pin, GPIO_TYPE, GPIO_SUBTYPE_FIRST, GPIO_SUBTYPE_LAST);
	if (steps < 4) {
		return false;
	}
	pon_known ? pon_inputs() : (void)0;
	pin_known ? volup_input() : (void)0;
	power.live = power.usable && !power.last;
	volume_down.live = volume_down.usable && !volume_down.last;
	volume_up.live = volume_up.usable && !volume_up.last;
	if (!power.usable && !volume_up.usable) {
		printf("buttons: nothing that can be read: no buttons\n");
		return false;
	}
	return steps >= 5;
}

static void
buttons_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;

	if (!bring_up(steps)) {
		printf("buttons: stopped (buttons_steps=%u)\n", steps);
		thread_terminate(current_thread());
		return;
	}
	printf("buttons: step 5: watching%s%s%s every %u ms; power is the side button\n", power.usable ? " power" : "",
	    volume_up.usable ? " volume-up" : "", volume_down.usable ? " volume-down" : "", POLL_MS);
	for (unsigned tick = 1; bus.failed < 1000 || bus.failed * 2 <= bus.reads; tick++) {
		poll_pon();
		poll_volup();
		(tick % MINUTE_TICKS == 0 || tick == 10 * 1000 / POLL_MS) ? say_summary() : (void)0;
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
	printf("buttons: more than half of %llu reads failed: giving up\n", bus.reads);
	thread_terminate(current_thread());
}

/* ---- test presses: boot-arg buttons_seq, sysctl kern.hakios.buttons_inject, boot-arg press_power ---------------- */

struct key_press {
	const char *name;
	uint32_t first, second;         /* HAKIOS_BUTTON_*; second 0: one key */
	uint32_t held_ms;
};

static const struct key_press key_presses[] = {
	{ "pwlong", HAKIOS_BUTTON_LOCK, 0, LONG_MS },
	{ "pw", HAKIOS_BUTTON_LOCK, 0, TAP_MS },
	{ "wake", HAKIOS_BUTTON_LOCK, 0, TAP_MS },
	{ "vu", HAKIOS_BUTTON_VOLUME_UP, 0, TAP_MS },
	{ "vd", HAKIOS_BUTTON_VOLUME_DOWN, 0, TAP_MS },
	{ "shot", HAKIOS_BUTTON_LOCK, HAKIOS_BUTTON_VOLUME_UP, TAP_MS },
};

static void
post_press(const struct key_press *k)
{
	uint64_t now = uptime_ms();

	injected++;
	printf("buttons: test press %s (%u%s%u) held %u ms at %llu.%03llu s\n", k->name, k->first, k->second ? "+" : "",
	    k->second, k->held_ms, now / 1000, now % 1000);
	hakios_touch_post(HAKIOS_BUTTON_DOWN, k->first, 0, 0);
	k->second ? hakios_touch_post(HAKIOS_BUTTON_DOWN, k->second, 0, 0) : (void)0;
	delay_for_interval(k->held_ms, NSEC_PER_MSEC);
	k->second ? hakios_touch_post(HAKIOS_BUTTON_UP, k->second, 0, 0) : (void)0;
	hakios_touch_post(HAKIOS_BUTTON_UP, k->first, 0, 0);
}

static const struct key_press *
find_press(const char *name, size_t length)
{
	for (size_t i = 0; i < sizeof(key_presses) / sizeof(key_presses[0]); i++) {
		if (strlen(key_presses[i].name) == length && strncmp(key_presses[i].name, name, length) == 0) {
			return &key_presses[i];
		}
	}
	return NULL;
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

/* "pw@200,vu@210": the steps in order of their seconds (the line is written in that order) */
static void
seq_thread(__unused void *parameter, __unused wait_result_t result)
{
	const char *cursor = seq_text;
	uint64_t start = uptime_ms();

	for (int n = 0; n < SEQ_MAX && *cursor != 0; n++) {
		const char *at = strchr(cursor, '@'), *end = strchr(cursor, ',');
		const struct key_press *k = at != NULL ? find_press(cursor, (size_t)(at - cursor)) : NULL;
		uint64_t when = at != NULL ? parse_decimal(at + 1) * 1000 : 0;

		if (k == NULL) {
			printf("buttons: buttons_seq: step %d is not KEY@SECONDS with a key this driver knows: sequence ends\n", n + 1);
			break;
		}
		while (uptime_ms() < start + when) {
			delay_for_interval(100, NSEC_PER_MSEC);
		}
		post_press(k);
		cursor = end != NULL ? end + 1 : "";
	}
	printf("buttons: buttons_seq done\n");
	thread_terminate(current_thread());
}

static void
start_thread(thread_continue_t body, void *parameter)
{
	thread_t thread;

	if (kernel_thread_start(body, parameter, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
seq_init(void)
{
	if (!PE_parse_boot_arg_str("buttons_seq", seq_text, sizeof(seq_text)) || seq_text[0] == 0) {
		return;
	}
	printf("buttons: buttons_seq=%s (seconds from now)\n", seq_text);
	start_thread(seq_thread, NULL);
}

static const struct key_press inject_presses[] = {
	{ "inject-power", HAKIOS_BUTTON_LOCK, 0, TAP_MS },
	{ "inject-volume-up", HAKIOS_BUTTON_VOLUME_UP, 0, TAP_MS },
	{ "inject-volume-down", HAKIOS_BUTTON_VOLUME_DOWN, 0, TAP_MS },
	{ "inject-power-long", HAKIOS_BUTTON_LOCK, 0, LONG_MS },
	{ "inject-volume-up-long", HAKIOS_BUTTON_VOLUME_UP, 0, LONG_MS },
	{ "inject-volume-down-long", HAKIOS_BUTTON_VOLUME_DOWN, 0, LONG_MS },
};

static void
inject_thread(void *parameter, __unused wait_result_t result)
{
	post_press(&inject_presses[(uintptr_t)parameter]);
	thread_terminate(current_thread());
}

SYSCTL_DECL(_kern_hakios);

static int
sysctl_buttons_inject SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, error = SYSCTL_OUT(req, &value, sizeof(value));
	uintptr_t index;

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	error = SYSCTL_IN(req, &value, sizeof(value));
	index = (uintptr_t)(value % 100 - 1) + (value >= 100 ? 3 : 0);
	if (error != 0 || value % 100 < 1 || value % 100 > 3 || value > 103) {
		return error != 0 ? error : EINVAL;
	}
	start_thread(inject_thread, (void *)index);
	return 0;
}

static int
sysctl_buttons_presses SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[160];

	snprintf(text, sizeof(text), "power %u volume-up %u volume-down %u injected %u reads %llu failed %llu", power.presses,
	    volume_up.presses, volume_down.presses, injected, bus.reads, bus.failed);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_INT(_kern_hakios, OID_AUTO, buttons_state, CTLFLAG_RD | CTLFLAG_LOCKED, &buttons_state, 0,
    "keys held now: bit 0 power, 1 volume up, 2 volume down");
SYSCTL_PROC(_kern_hakios, OID_AUTO, buttons_presses, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctl_buttons_presses, "A", "presses counted since boot, and the PMIC reads");
SYSCTL_PROC(_kern_hakios, OID_AUTO, buttons_inject, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    sysctl_buttons_inject, "I", "write 1 power, 2 volume up, 3 volume down (+100: held 3.5 s): a test press now");

/* HakiOS's press_power=S (press_power_ms=): power at S s and at S + 10 s */
static void
press_power_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t seconds = (uint32_t)(uintptr_t)parameter, held = TAP_MS;
	struct key_press k = { "press_power", HAKIOS_BUTTON_LOCK, 0, TAP_MS };

	PE_parse_boot_argn("press_power_ms", &held, sizeof(held));
	k.held_ms = held;
	delay_for_interval(seconds, NSEC_PER_SEC);
	post_press(&k);
	delay_for_interval(10, NSEC_PER_SEC);
	post_press(&k);
	thread_terminate(current_thread());
}

void
hakios_buttons_init(void)
{
	uint32_t enabled = 0, steps = 5, touch_screen = 0, seconds = 0;

	if (PE_parse_boot_argn("press_power", &seconds, sizeof(seconds)) && seconds != 0) {
		start_thread(press_power_thread, (void *)(uintptr_t)seconds);
	}
	seq_init();
	/* on with the touch screen (lisa=1 gives touchscreen=1) unless buttons=0 */
	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		enabled = PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0;
	}
	PE_parse_boot_argn("buttons_steps", &steps, sizeof(steps));
	if (!enabled || steps == 0) {
		return;
	}
	start_thread(buttons_thread, (void *)(uintptr_t)steps);
}
