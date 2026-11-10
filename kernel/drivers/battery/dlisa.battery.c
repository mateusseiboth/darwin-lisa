/*
 * darwin-lisa: the battery of lisa (Xiaomi 11 Lite 5G NE, SM7325), read straight from the PMIC over SPMI.
 *
 * On lisa the fuel gauge and the charger belong to the audio DSP's battery manager (pmic_glink / qcom_battmgr in
 * postmarketOS), which needs the ADSP firmware, its loader and GLINK. Until that is ported, the battery's voltage is
 * what can be had without it: the PMK8350's ADC (ADC7, "qcom,spmi-adc7", slave 0, peripheral 0x31) converts the
 * PM7325B's VBAT_SNS (1/3 prescale) and USB_IN_V (1/16) channels on request. The charge is ESTIMATED from the voltage
 * (the xnuloader's table), and a charger is "there" when the USB input carries more than 4 V. Every reading goes to
 * hakios_battery_report() -> HakiosBattery (iokit/Kernel/hakios.battery.cpp), an IOPMPowerSource, which powerd reads
 * and SpringBoard's battery follows.
 *
 * The real fuel gauge (adsp=1 battmgr=1, kernel/drivers/adsp/hakios.adsp.c) reports through hakios_battery_report()
 * below as well. While its reports are fresh (the last one less than DSP_FRESH_SECONDS old) the ADC's estimate is not
 * passed on: one source at a time reaches HakiosBattery, and the DSP's wins. The sysctls show whichever was passed on
 * (kern.hakios.battery_source: 1 the ADC's estimate, 2 the DSP's fuel gauge).
 *
 * Boot-args:
 *   battery=1            on (off by default)
 *   battery_secs=N       a reading every N seconds (default 30)
 *   battery_adc=0        no conversions: only the arbiter's channel table and the ADC's registers are read (no write)
 *   battery_dump=1       the ADC's registers before and after each conversion
 *
 * The ADC7 registers (Linux drivers/iio/adc/qcom-spmi-adc5.c): APP_SID 0x40 [3:0] the slave of the channel; DIG_PARAM
 * 0x42 (calibration [5:4], decimation [3:2]), FAST_AVG_CTL 0x43 [2:0], CH_SEL_CTL 0x44, DELAY_CTL 0x45 [3:0], EN_CTL1
 * 0x46 [7] enable, CONV_REQ 0x47 [7]; STATUS1 0x08 [0] end of conversion, [7] fault; DATA0/1 0x50/0x51. A code is
 * code * 1.875 V / 0x70e4 at the pin (codes above 0x7fff are below 0 V).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* ---- the PMIC arbiter (version 5), as drivers/input/hakios.buttons.c and drivers/camera/hakios.torch.c ---- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* reads: 0x80 per channel for our system (the first) */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL    /* writes: 0x10000 per channel */
#define ARB_SYSTEM              0               /* the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000

/* ---- the ADC ---- */

#define ADC_ID                  0x031           /* slave 0 (PMK8350), peripheral 0x31: registers 0x3100 */
#define ADC_TYPE                0x04            /* peripheral type, subtype */
#define ADC_STATUS1             0x08
#define ADC_STATUS1_EOC         0x01
#define ADC_STATUS1_FAULT       0x80
#define ADC_APP_SID             0x40
#define ADC_DIG_PARAM           0x42
#define ADC_EN_CTL1             0x46
#define ADC_EN_CTL1_ENABLE      0x80
#define ADC_CONV_REQ            0x47
#define ADC_CONV_REQ_REQ        0x80
#define ADC_DATA0               0x50
#define ADC_FULL_SCALE          0x70e4
#define ADC_VREF_UV             1875000LL
#define ADC_CAL_ABSOLUTE        2
#define ADC_DECIMATION_DEFAULT  2               /* 1360 on ADC7 */

/* channels: slave << 8 | channel (dt-bindings/iio/qcom,spmi-adc7-pm8350b.h, PM7325B = PM8350B at slave 3) */
struct adc_channel {
	const char *name;
	uint16_t id;
	uint32_t prescale;              /* the voltage at the pin is 1/prescale of the signal */
};
static const struct adc_channel CH_VBAT = { "VBAT_SNS", 0x38f, 3 };
static const struct adc_channel CH_USB_IN = { "USB_IN_V", 0x311, 16 };
static const struct adc_channel CH_VPH = { "VPH_PWR", 0x38e, 3 };

static vm_offset_t arb_core, arb_cnfg, arb_observer, adc_write;
static uint16_t adc_channel_number;
static bool adc_writable;
static uint32_t dump_registers;

static int battery_soc_percent = -1, battery_vbat_mv = -1, battery_usb_mv = -1, battery_vph_mv = -1, battery_external = -1;
static int battery_last_code = -1;
static int battery_source, battery_current_ma, battery_temp_tenths = -2730, battery_status = -1, battery_health = -1;
static int battery_cycles = -1;
static uint64_t dsp_reported_at;        /* mach_absolute_time() of the DSP's last report; 0: none yet */
#define DSP_FRESH_SECONDS       60

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_soc_percent, 0,
    "battery charge 0..100, estimated from its voltage, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_vbat_mv, 0,
    "battery voltage (PM7325B VBAT_SNS) in mV, or -1");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_usb_mv, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_usb_mv, 0,
    "USB input voltage (PM7325B USB_IN_V) in mV, or -1");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_vph_mv, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_vph_mv, 0,
    "system supply (VPH_PWR) in mV, or -1");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_external, 0,
    "1 if the USB input carries a supply, -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_source, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_source, 0,
    "what the battery's state comes from: 0 nothing yet, 1 the ADC's voltage estimate, 2 the ADSP's fuel gauge");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_current_ma, 0,
    "battery current in mA, more than 0 while it takes charge (fuel gauge only; 0 otherwise)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temp_tenths, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_temp_tenths, 0,
    "battery temperature in tenths of a degree C (fuel gauge only; -2730 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_status, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_status, 0,
    "Linux POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full; -1 unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_health, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_health, 0,
    "Linux POWER_SUPPLY_HEALTH_*: 1 good, 2 overheat, 3 dead, 6 cold, ...; -1 unknown (fuel gauge only)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED, &battery_cycles, 0,
    "charge cycles (fuel gauge only; -1 unknown)");

static bool
is_dsp_fresh(void)
{
	uint64_t nanoseconds;

	if (dsp_reported_at == 0) {
		return false;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - dsp_reported_at, &nanoseconds);
	return nanoseconds < DSP_FRESH_SECONDS * NSEC_PER_SEC;
}

static void report(const struct hakios_battery_state *b, bool from_dsp);

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	}
	if (status & STATUS_DENIED) {
		return "the arbiter refuses";
	}
	if (status & STATUS_FAILURE) {
		return "the transfer failed";
	}
	if (status & STATUS_DROPPED) {
		return "the transfer was dropped";
	}
	return NULL;
}

/* `count` (1..8) registers of the ADC from `reg` on, through the observer channel */
static const char *
adc_read(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x80UL * adc_channel_number;
	uint32_t data[2];
	const char *problem;

	hakios_spmi_lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	hakios_spmi_unlock();
	for (uint32_t i = 0; problem == NULL && i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return problem;
}

/* `count` (1..4) registers of the ADC from `reg` on, through its write channel (ours only if the arbiter says so) */
static const char *
adc_write_regs(uint8_t reg, const uint8_t *bytes, uint32_t count)
{
	uint32_t word = 0;
	const char *problem = NULL;

	if (!adc_writable) {
		return "the ADC's channel is not ours to write";
	}
	for (uint32_t i = 0; i < count; i++) {
		word |= (uint32_t)bytes[i] << (8 * i);
	}
	for (int tries = 0; tries < 3; tries++) {
		hakios_spmi_lock();
		wr(adc_write + CHANNEL_WDATA0, word);
		wr(adc_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4 | (count - 1));
		problem = wait_done(adc_write);
		hakios_spmi_unlock();
		if (problem == NULL || strcmp(problem, "the transfer was dropped") != 0) {
			break;
		}
	}
	return problem;
}

static void
dump(const char *when)
{
	uint8_t a[8] = { 0 }, b[8] = { 0 }, c[8] = { 0 };

	adc_read(0x00, a, 8);
	adc_read(0x40, b, 8);
	adc_read(0x50, c, 2);
	printf("battery: adc %s: 00: %02x %02x %02x %02x %02x %02x %02x %02x  40: %02x %02x %02x %02x %02x %02x %02x %02x  50: %02x %02x\n",
	    when, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], c[0], c[1]);
}

/* One conversion: the signal in microvolts, or -1. */
static int64_t
convert(const struct adc_channel *ch)
{
	uint8_t sid = (uint8_t)(ch->id >> 8), block[4], status = 0, data[2], enable;
	const char *problem;
	uint16_t code;
	int64_t uv;

	if ((problem = adc_read(ADC_APP_SID, block, 1)) != NULL) {
		goto failed;
	}
	block[0] = (uint8_t)((block[0] & ~0x0f) | (sid & 0x0f));
	if ((problem = adc_write_regs(ADC_APP_SID, block, 1)) != NULL) {
		goto failed;
	}
	if ((problem = adc_read(ADC_DIG_PARAM, block, 4)) != NULL) {
		goto failed;
	}
	block[0] = (uint8_t)((block[0] & ~0x7c) | (ADC_CAL_ABSOLUTE << 4) | (ADC_DECIMATION_DEFAULT << 2));   /* cal val 0 */
	block[1] = (uint8_t)(block[1] & ~0x07);         /* one sample */
	block[2] = (uint8_t)(ch->id & 0xff);            /* the channel */
	block[3] = (uint8_t)(block[3] & ~0x0f);         /* no settling delay */
	enable = ADC_EN_CTL1_ENABLE;
	if ((problem = adc_write_regs(ADC_EN_CTL1, &enable, 1)) != NULL ||
	    (problem = adc_write_regs(ADC_DIG_PARAM, block, 4)) != NULL) {
		goto failed;
	}
	enable = ADC_CONV_REQ_REQ;
	if ((problem = adc_write_regs(ADC_CONV_REQ, &enable, 1)) != NULL) {
		goto failed;
	}
	for (int ms = 0; ms < 50; ms++) {
		sleep_ms(1);
		if ((problem = adc_read(ADC_STATUS1, &status, 1)) != NULL) {
			goto failed;
		}
		if (status & ADC_STATUS1_EOC) {
			break;
		}
	}
	if (dump_registers) {
		dump(ch->name);
	}
	if (!(status & ADC_STATUS1_EOC) || (status & ADC_STATUS1_FAULT)) {
		printf("battery: %s: no conversion (status %#x)\n", ch->name, status);
		return -1;
	}
	if ((problem = adc_read(ADC_DATA0, data, 2)) != NULL) {
		goto failed;
	}
	code = (uint16_t)(data[0] | data[1] << 8);
	battery_last_code = code;
	uv = code > 0x7fff ? 0 : (int64_t)code * ADC_VREF_UV / ADC_FULL_SCALE * ch->prescale;
	printf("battery: %s: code %#06x status %#x -> %lld mV\n", ch->name, code, status, uv / 1000);
	return uv;
failed:
	printf("battery: %s: %s\n", ch->name, problem);
	return -1;
}

/* The xnuloader's table (soc_from_voltage): 3300 mV 0%, 3700 10%, 3900 50%, 4200 90%, 4400 100%, straight between. */
static int
percent_from_mv(int mv)
{
	static const int points[][2] = { { 3300, 0 }, { 3700, 10 }, { 3900, 50 }, { 4200, 90 }, { 4400, 100 } };
	const size_t n = sizeof(points) / sizeof(points[0]);

	if (mv <= points[0][0]) {
		return 0;
	}
	for (size_t i = 1; i < n; i++) {
		if (mv <= points[i][0]) {
			return points[i - 1][1] + (mv - points[i - 1][0]) * (points[i][1] - points[i - 1][1]) /
			       (points[i][0] - points[i - 1][0]);
		}
	}
	return 100;
}

#define TREND   6               /* readings: 3 minutes at the default 30 s */

/* Is VBAT at least where it was TREND readings ago? Not known (false) until there are that many. */
static bool
is_vbat_rising(int mv)
{
	static int history[TREND];
	static unsigned count;
	int then = history[count % TREND];
	bool known = count >= TREND;

	history[count++ % TREND] = mv;
	return known && mv >= then;
}

static void
reading(void)
{
	struct hakios_battery_state b = { .size = sizeof(b) };
	int64_t vbat = convert(&CH_VBAT), usb = convert(&CH_USB_IN), vph = convert(&CH_VPH);

	battery_vbat_mv = vbat < 0 ? -1 : (int)(vbat / 1000);
	battery_usb_mv = usb < 0 ? -1 : (int)(usb / 1000);
	battery_vph_mv = vph < 0 ? -1 : (int)(vph / 1000);
	/* a battery reads 2.5..4.6 V; anything else is a conversion we got wrong, not a battery */
	if (battery_vbat_mv < 2500 || battery_vbat_mv > 4600) {
		printf("battery: VBAT %d mV is not a battery's; nothing reported\n", battery_vbat_mv);
		return;
	}
	battery_external = battery_usb_mv > 4000;
	battery_soc_percent = percent_from_mv(battery_vbat_mv);
	b.percent_hundredths = battery_soc_percent * 100;
	b.voltage_uv = battery_vbat_mv * 1000;
	b.current_ua = 0;                       /* not known without the fuel gauge */
	b.temperature_hundredths = 2500;        /* not known without the fuel gauge: a neutral 25 C */
	b.present = 1;
	b.external = battery_external;
	/*
	 * Charging is not something the ADC says. Without the DSP nothing manages the PMIC's charger, and on the phone
	 * VBAT was seen to fall on USB (4142 -> 4086 mV in 4 minutes at 7 cores). So: on a supply, "charging" only while
	 * VBAT is not lower than TREND readings ago, else "not charging" (on a charger, the system takes more than it).
	 */
	b.status = !battery_external ? 2 : battery_soc_percent >= 100 ? 4 : is_vbat_rising(battery_vbat_mv) ? 1 : 3;
	b.usb_type = battery_external ? 1 : 0;
	b.usb_voltage_uv = battery_usb_mv > 0 ? battery_usb_mv * 1000 : 0;
	b.usb_current_ua = battery_external ? 500000 : 0;
	printf("battery: VBAT %d mV -> %d%% (estimated from the voltage), USB in %d mV (%s), VPH %d mV, status %d%s\n",
	    battery_vbat_mv, battery_soc_percent, battery_usb_mv, battery_external ? "a charger or a computer" : "nothing",
	    battery_vph_mv, b.status, is_dsp_fresh() ? "; not passed on: the DSP's fuel gauge reports" : "");
	report(&b, false);
}

static bool
find_adc_channel(void)
{
	uint32_t version = rd(arb_core + ARB_VERSION), channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	bool found = false;

	printf("battery: PMIC arbiter version %#x, %u channels\n", version, channels);
	if (version < 0x50000000u || version >= 0x70000000u) {
		printf("battery: not the arbiter version 5 this driver knows\n");
		return false;
	}
	channels = channels > ARB_MAX_CHANNELS ? ARB_MAX_CHANNELS : channels;
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != ADC_ID) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		printf("battery: channel %u is the PMK8350's ADC (%#x), written by system %u\n", n, map, owner);
		if (!found || owner == ARB_SYSTEM) {
			found = true;
			adc_channel_number = (uint16_t)n;
			adc_writable = owner == ARB_SYSTEM;
		}
	}
	if (!found) {
		printf("battery: no channel of the arbiter is the PMK8350's ADC (slave 0, peripheral 0x31)\n");
	}
	return found;
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 30, use_adc = 1;
	uint8_t type[2] = { 0 };
	const char *problem;

	PE_parse_boot_argn("battery_secs", &secs, sizeof(secs));
	PE_parse_boot_argn("battery_adc", &use_adc, sizeof(use_adc));
	PE_parse_boot_argn("battery_dump", &dump_registers, sizeof(dump_registers));
	secs = secs < 2 ? 2 : secs;
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS + 0x10000UL * ARB_SYSTEM, ARB_OBSERVER_SIZE);
	if (!find_adc_channel()) {
		thread_terminate(current_thread());
		return;
	}
	adc_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * adc_channel_number, 0x1000);
	problem = adc_read(ADC_TYPE, type, 2);
	printf("battery: the ADC (channel %u, %s): type %#x subtype %#x%s%s\n", adc_channel_number,
	    adc_writable ? "ours to write" : "NOT ours to write", type[0], type[1], problem ? ": " : "", problem ? problem : "");
	dump("as found");
	if (!use_adc) {
		printf("battery: battery_adc=0: no conversions\n");
		thread_terminate(current_thread());
		return;
	}
	for (;;) {
		reading();
		sleep_ms(secs * 1000);
	}
}

void
hakios_battery_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

int
hakios_battery_temperature(void)
{
	/* the fuel gauge's, while it is fresh; else not known (the ADC path has no temperature) */
	return is_dsp_fresh() ? battery_temp_tenths / 10 : -273;
}

extern void HakiosBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/hakios.battery.cpp */

static void
report(const struct hakios_battery_state *b, bool from_dsp)
{
	if (!from_dsp && is_dsp_fresh()) {
		return;
	}
	if (from_dsp) {
		dsp_reported_at = mach_absolute_time();
		battery_soc_percent = (int)(((int64_t)b->percent_hundredths + 50) / 100);
		battery_vbat_mv = b->voltage_uv / 1000;
		battery_usb_mv = b->external ? b->usb_voltage_uv / 1000 : 0;
		battery_external = b->external != 0;
		battery_current_ma = b->current_ua / 1000;
		battery_temp_tenths = (int)(((int64_t)b->temperature_hundredths + 5) / 10);
		battery_cycles = b->cycles;
	}
	battery_source = from_dsp ? 2 : 1;
	battery_status = b->status;
	HakiosBatteryReport(b, sizeof(*b));
}

/* The DSP's fuel gauge (kernel/drivers/adsp/hakios.adsp.c, battmgr=1), and anybody else with a whole state. */
void
hakios_battery_report(const struct hakios_battery_state *b)
{
	report(b, true);
}

/* Linux's POWER_SUPPLY_HEALTH_* from the fuel gauge (hakios.adsp.c); HakiosBattery reads it with the next report. */
void dlisa_battery_note_health(int linux_health);
int dlisa_battery_health(void);

void
dlisa_battery_note_health(int linux_health)
{
	battery_health = linux_health;
}

int
dlisa_battery_health(void)
{
	return is_dsp_fresh() ? battery_health : -1;
}
