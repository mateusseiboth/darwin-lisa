/*
 * darwin-lisa: the two speaker amplifiers of the Xiaomi 11 Lite 5G NE (lisa), NXP TFA9873, for dlisa.sound.c.
 *
 * Where they are (pmOS sm7325-xiaomi-lisa.dtb, i2c@98c000, and the stock overlay's fragment on qupv3_se3_i2c):
 *   I2C        QUP serial engine 3 (0x98c000), GPIO 12 data and 13 clock (pinctrl qup-i2c3-data-clk, function qup03);
 *              driven by hand here as dlisa.haptic.c does (HakiOS's hakios.speaker.c way): the two pins taken for each
 *              transaction and given back as they were found. 100 kHz at most.
 *   0x34       pmOS "Earpiece", sound-channel 0 (left); the stock tree's tfa98xx_1: reset GPIO 2, interrupt GPIO 102,
 *              and a speaker/receiver switch on GPIO 101 (spk-sw-gpio). The top unit.
 *   0x35       pmOS "Speaker", sound-channel 1 (right); stock tfa98xx_2: reset GPIO 6, interrupt GPIO 24. The bottom unit.
 *   reset      active high (the stock "active" pin state drives GPIO 2 low). pmOS's driver never touches it, so it is
 *              only looked at here unless boot-arg sound_amp_reset=1 asks for a pulse (high 2 ms, then low).
 *
 * What is written is what pmOS's sound/soc/codecs/tfa9872.c (Linux 7.2.9) writes and nothing else: no NXP container
 * (tfa98xx.cnt), no DSP firmware, no speaker calibration. Registers are 8-bit addresses, 16-bit values, most
 * significant byte first (regmap: reg_bits 8, val_bits 16). At probe: the chip's revision (0x03) checked against
 * pmOS's list; a soft reset (SYS_CTRL0 = I2CR); the DC-DC converter bit (pmOS sets it when the tree has no DC-DC
 * properties, as lisa's has none; boot-arg sound_dcdc=0 clears it); DPSA off; "TDM" 16-bit slots, 2 slots; the speaker
 * channel from the I2S slot `sound-channel'; no current or voltage sense; power up; MANSCONF. Then ASoC's DAPM powers
 * the chip down while nothing plays (PWUP, SYS_CTRL0 bit 0 inverted) and up for a stream, and mute_stream sets AMPE.
 * Here: dlisa_tfa_prepare() is the probe ending powered down and muted, dlisa_tfa_start() is DAPM's power-up and the
 * unmute, dlisa_tfa_stop() the mute and the power-down. The chips have no volume register in pmOS's driver: the level
 * is set on the samples (dlisa.sound.c).
 *
 * Guards: every write is read back (the soft reset excepted); a chip that does not acknowledge, a register that does
 * not read back, a revision pmOS does not know, or a SYS_CTRL0 that changes by itself while playing: both chips are
 * muted and powered down and the caller is told. The status registers 0x10 and 0x11 (the stock driver's "STATUS_FLAG0"
 * and "STATUS_FLAG1") are logged raw, never interpreted: their bit layout for the TFA9873 is not known here.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "dlisa.tfa9873.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins ------------------------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3u
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 12
#define PIN_SCL                 13
#define PIN_RESET_TOP           2
#define PIN_RESET_BOTTOM        6
#define PIN_SPK_SW              101
#define PIN_INT_TOP             102
#define PIN_INT_BOTTOM          24

static vm_offset_t tlmm;

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
map_tlmm(void)
{
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
}

static void
show_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = {
		{ PIN_SDA, "I2C data (SE3)" }, { PIN_SCL, "I2C clock (SE3)" }, { PIN_RESET_TOP, "reset of 0x34" },
		{ PIN_RESET_BOTTOM, "reset of 0x35" }, { PIN_SPK_SW, "speaker switch of 0x34 (stock)" },
		{ PIN_INT_TOP, "interrupt of 0x34" }, { PIN_INT_BOTTOM, "interrupt of 0x35" },
	};

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("sound: tfa: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

/* ---- I2C by hand (dlisa.haptic.c) ---------------------------------------------------------------------------- */

static LCK_GRP_DECLARE(tfa_lock_group, "dlisa-tfa9873");
static LCK_MTX_DECLARE(tfa_lock, &tfa_lock_group);

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level_of(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

static uint32_t taken_cfg[2], taken_io[2];

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (level_of(PIN_SDA) && level_of(PIN_SCL)) {
		return true;
	}
	bus_give_back();
	say("sound: tfa: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
	    level_of(PIN_SDA), PIN_SCL, level_of(PIN_SCL));
	return false;
}

/* One 16-bit register (most significant byte first, as regmap's val_bits 16). */
static bool
reg_read(uint8_t address, uint8_t reg, uint16_t *value)
{
	bool answered;
	uint8_t high = 0, low = 0;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg);
	answered ? i2c_start() : (void)0;
	answered = answered && i2c_write((uint8_t)(address << 1 | 1));
	if (answered) {
		high = i2c_read(true);
		low = i2c_read(false);
	}
	i2c_stop();
	bus_give_back();
	*value = (uint16_t)(high << 8 | low);
	return answered;
}

static bool
reg_write(uint8_t address, uint8_t reg, uint16_t value)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg) && i2c_write((uint8_t)(value >> 8)) &&
	    i2c_write((uint8_t)value);
	i2c_stop();
	bus_give_back();
	return answered;
}

/* ---- the chips (tfa9872.c) ----------------------------------------------------------------------------------- */

#define SYS_CTRL0               0x00
#define SYS_CTRL0_PWDN          (1u << 0)
#define SYS_CTRL0_I2CR          (1u << 1)
#define SYS_CTRL0_AMPE          (1u << 3)
#define SYS_CTRL0_DCDC          (1u << 4)
#define SYS_CTRL1               0x01
#define SYS_CTRL1_MANSCONF      (1u << 2)
#define REV                     0x03
#define STATUS_FLAGS0           0x10            /* logged raw */
#define STATUS_FLAGS1           0x11            /* logged raw */
#define TDM_CFG0                0x20
#define TDM_CFG0_FSBCLKS        0xf000u
#define TDM_CFG1                0x21
#define TDM_CFG1_NSLOTS         0x000fu
#define TDM_CFG1_SLOTBITS       0x01f0u
#define TDM_CFG2                0x22
#define TDM_CFG2_SWIDTH         0x007cu
#define TDM_CFG3                0x23
#define TDM_CFG3_SPKE           (1u << 0)
#define TDM_CFG3_DCE            (1u << 1)
#define TDM_CFG3_CSE            (1u << 3)
#define TDM_CFG3_VSE            (1u << 4)
#define TDM_CFG6                0x26
#define TDM_CFG6_SPKS           0x000fu
#define AUDIO_CTRL              0x51
#define AUDIO_CTRL_DPSA         (1u << 7)
#define MODE1_DET1              0x64
#define MODE1_DET1_LPM1MODE     0xc000u
/* what this driver keeps an eye on in SYS_CTRL0 while the chips play: power, mute, DC-DC */
#define SYS_CTRL0_WATCHED       (SYS_CTRL0_PWDN | SYS_CTRL0_AMPE | SYS_CTRL0_DCDC)

static struct amp {
	uint8_t address;
	uint8_t channel;                /* the I2S slot it plays: pmOS's sound-channel */
	unsigned reset_pin;
	uint32_t bit;                   /* DLISA_TFA_* */
	const char *name;
	uint16_t rev, flags0, flags1, ctrl0;
	bool found;
} amps[] = {
	{ 0x34, 0, PIN_RESET_TOP, DLISA_TFA_TOP, "0x34 (top, left)", 0, 0, 0, 0, false },
	{ 0x35, 1, PIN_RESET_BOTTOM, DLISA_TFA_BOTTOM, "0x35 (bottom, right)", 0, 0, 0, 0, false },
};
#define AMPS                    (sizeof(amps) / sizeof(amps[0]))

static uint32_t mask = DLISA_TFA_TOP | DLISA_TFA_BOTTOM;
static bool reset_pulse, dcdc = true, broken, on;
static uint32_t flag_lines;             /* status lines logged so far (at most 40) */
static char state_text[96] = "not looked at";

static bool
wanted(const struct amp *a)
{
	return (mask & a->bit) != 0;
}

void
dlisa_tfa_configure(uint32_t amps_mask, bool pulse_reset, bool dcdc_on)
{
	mask = amps_mask & (DLISA_TFA_TOP | DLISA_TFA_BOTTOM);
	reset_pulse = pulse_reset;
	dcdc = dcdc_on;
}

static bool
get(struct amp *a, uint8_t reg, uint16_t *value)
{
	if (!reg_read(a->address, reg, value)) {
		say("sound: tfa: %s register %#04x: NO ANSWER\n", a->name, reg);
		return false;
	}
	return true;
}

/* write, then read back: false (and said) if either is not acknowledged or the value is not there */
static bool
put(struct amp *a, uint8_t reg, uint16_t value)
{
	uint16_t after = 0;

	if (!reg_write(a->address, reg, value) || !reg_read(a->address, reg, &after)) {
		say("sound: tfa: %s register %#04x <- %#06x: NOT ACKNOWLEDGED\n", a->name, reg, value);
		return false;
	}
	if (after != value) {
		say("sound: tfa: %s register %#04x <- %#06x: READS %#06x\n", a->name, reg, value, after);
		return false;
	}
	return true;
}

static bool
put_bits(struct amp *a, uint8_t reg, uint16_t bits, uint16_t value)
{
	uint16_t now = 0;

	if (!get(a, reg, &now)) {
		return false;
	}
	return (now & bits) == (value & bits) || put(a, reg, (uint16_t)((now & ~bits) | (value & bits)));
}

static bool
known_revision(uint16_t rev)
{
	/* pmOS's list (tfa987x_i2c_probe); the TFA9873 is 0x0b73 */
	return rev == 0x0b73 || rev == 0x1b72 || rev == 0x2b72 || rev == 0x3b72 || rev == 0x0c74;
}

/* The status registers, logged when they change (40 lines at most in a run). */
static void
flags(struct amp *a, const char *when)
{
	uint16_t f0 = 0, f1 = 0, c0 = 0;

	if (!reg_read(a->address, STATUS_FLAGS0, &f0) || !reg_read(a->address, STATUS_FLAGS1, &f1) ||
	    !reg_read(a->address, SYS_CTRL0, &c0)) {
		return;
	}
	if ((f0 != a->flags0 || f1 != a->flags1 || c0 != a->ctrl0) && flag_lines < 40) {
		flag_lines++;
		say("sound: tfa: %s %s: SYS_CTRL0 %#06x, status 0x10 %#06x, 0x11 %#06x\n", a->name, when, c0, f0, f1);
	}
	a->flags0 = f0;
	a->flags1 = f1;
	a->ctrl0 = c0;
}

/* Muted and powered down, whatever was asked before: the way out of every failure. With tfa_lock held. */
static void
all_off(const char *why)
{
	for (unsigned i = 0; i < AMPS; i++) {
		uint16_t c0 = 0;

		if (!amps[i].found) {
			continue;
		}
		if (reg_read(amps[i].address, SYS_CTRL0, &c0)) {
			(void)reg_write(amps[i].address, SYS_CTRL0, (uint16_t)((c0 & ~SYS_CTRL0_AMPE) | SYS_CTRL0_PWDN));
			(void)reg_read(amps[i].address, SYS_CTRL0, &c0);
		}
		say("sound: tfa: %s muted and powered down (%s): SYS_CTRL0 now %#06x\n", amps[i].name, why, c0);
	}
	on = false;
}

static void
give_up(const char *why)
{
	broken = true;
	say("sound: tfa: %s: BOTH AMPLIFIERS OFF, and they are not started again in this run\n", why);
	all_off(why);
	snprintf(state_text, sizeof(state_text), "given up: %s", why);
}

bool
dlisa_tfa_look(void)
{
	bool ok = true;

	map_tlmm();
	lck_mtx_lock(&tfa_lock);
	show_pins();
	if (reset_pulse) {
		for (unsigned i = 0; i < AMPS; i++) {
			if (!wanted(&amps[i])) {
				continue;
			}
			say("sound: tfa: %s: reset pulsed (gpio%u high 2 ms, then low), as NXP's driver does; sound_amp_reset=1\n",
			    amps[i].name, amps[i].reset_pin);
			wr(GPIO_IO(amps[i].reset_pin), 2);
			wr(GPIO_CFG(amps[i].reset_pin), CFG_OUTPUT);
			sleep_ms(2);
			wr(GPIO_IO(amps[i].reset_pin), 0);
		}
		sleep_ms(10);
	}
	for (unsigned i = 0; i < AMPS; i++) {
		struct amp *a = &amps[i];

		if (!wanted(a)) {
			say("sound: tfa: %s left alone (sound_amps=%u)\n", a->name, mask);
			continue;
		}
		a->found = reg_read(a->address, REV, &a->rev);
		say("sound: tfa: %s: revision %#06x%s\n", a->name, a->rev, !a->found ? ": NOBODY ANSWERS (in reset? try "
		    "sound_amp_reset=1)" : known_revision(a->rev) ? (a->rev == 0x0b73 ? " (TFA9873)" : " (known to pmOS, NOT a TFA9873)") :
		    ": NOT ONE pmOS's driver knows");
		ok = ok && a->found && known_revision(a->rev);
		if (a->found) {
			flags(a, "as found");
		}
	}
	snprintf(state_text, sizeof(state_text), "looked at: 0x34 rev %#x%s, 0x35 rev %#x%s", amps[0].rev,
	    amps[0].found ? "" : " (absent)", amps[1].rev, amps[1].found ? "" : " (absent)");
	lck_mtx_unlock(&tfa_lock);
	return ok && mask != 0;
}

/* pmOS's probe (tfa987x_i2c_probe), ending muted and powered down as DAPM leaves an idle chip */
static bool
prepare_one(struct amp *a)
{
	uint16_t c0 = 0;
	bool ok;

	say("sound: tfa: %s: soft reset, then pmOS's setup (DC-DC %s, DPSA off, 2 slots of 16 bits, speaker on slot %u, no "
	    "sense data); muted and powered down\n", a->name, dcdc ? "on" : "OFF (sound_dcdc=0)", a->channel);
	if (!reg_write(a->address, SYS_CTRL0, SYS_CTRL0_I2CR)) {
		say("sound: tfa: %s: the soft reset was not acknowledged\n", a->name);
		return false;
	}
	sleep_ms(2);
	if (!get(a, SYS_CTRL0, &c0)) {
		return false;
	}
	say("sound: tfa: %s: after the soft reset SYS_CTRL0 is %#06x\n", a->name, c0);
	/* (the mute first: whatever the reset left, nothing is to be heard until dlisa_tfa_start()) */
	ok = put_bits(a, SYS_CTRL0, SYS_CTRL0_AMPE | SYS_CTRL0_PWDN, SYS_CTRL0_PWDN);
	ok = ok && put_bits(a, SYS_CTRL0, SYS_CTRL0_DCDC, dcdc ? SYS_CTRL0_DCDC : 0);
	ok = ok && put_bits(a, AUDIO_CTRL, AUDIO_CTRL_DPSA, 0);
	ok = ok && put_bits(a, TDM_CFG0, TDM_CFG0_FSBCLKS, 0);
	ok = ok && put_bits(a, TDM_CFG1, TDM_CFG1_NSLOTS | TDM_CFG1_SLOTBITS, (uint16_t)(1u | 15u << 4));
	ok = ok && put_bits(a, TDM_CFG2, TDM_CFG2_SWIDTH, (uint16_t)(15u << 2));
	ok = ok && put_bits(a, TDM_CFG3, TDM_CFG3_SPKE | TDM_CFG3_DCE | TDM_CFG3_CSE | TDM_CFG3_VSE, TDM_CFG3_SPKE);
	ok = ok && put_bits(a, TDM_CFG6, TDM_CFG6_SPKS, a->channel);
	if (ok && (a->rev & 0xff) == 0x72) {
		ok = put_bits(a, MODE1_DET1, MODE1_DET1_LPM1MODE, 1u << 14);
	}
	if (ok) {
		flags(a, "set up, powered down");
	}
	return ok;
}

bool
dlisa_tfa_prepare(void)
{
	bool ok = true;

	lck_mtx_lock(&tfa_lock);
	if (broken) {
		lck_mtx_unlock(&tfa_lock);
		return false;
	}
	for (unsigned i = 0; ok && i < AMPS; i++) {
		if (wanted(&amps[i])) {
			ok = amps[i].found && known_revision(amps[i].rev) && prepare_one(&amps[i]);
		}
	}
	if (!ok) {
		give_up("the setup failed");
	} else {
		snprintf(state_text, sizeof(state_text), "set up, muted and powered down");
	}
	lck_mtx_unlock(&tfa_lock);
	return ok;
}

/* DAPM's power-up (PWUP: PWDN 0), the probe's MANSCONF after it, then mute_stream(0): AMPE 1 */
bool
dlisa_tfa_start(void)
{
	bool ok = true;

	lck_mtx_lock(&tfa_lock);
	if (broken) {
		lck_mtx_unlock(&tfa_lock);
		return false;
	}
	for (unsigned i = 0; ok && i < AMPS; i++) {
		if (wanted(&amps[i])) {
			say("sound: tfa: %s: powered up (PWDN 0, MANSCONF 1)\n", amps[i].name);
			ok = put_bits(&amps[i], SYS_CTRL0, SYS_CTRL0_PWDN, 0) && put_bits(&amps[i], SYS_CTRL1, SYS_CTRL1_MANSCONF,
			    SYS_CTRL1_MANSCONF);
		}
	}
	if (ok) {
		sleep_ms(10);
		for (unsigned i = 0; ok && i < AMPS; i++) {
			if (wanted(&amps[i])) {
				flags(&amps[i], "powered up, muted");
				say("sound: tfa: %s: unmuted (AMPE 1)\n", amps[i].name);
				ok = put_bits(&amps[i], SYS_CTRL0, SYS_CTRL0_AMPE, SYS_CTRL0_AMPE);
			}
		}
	}
	on = ok;
	if (!ok) {
		give_up("the start failed");
	} else {
		snprintf(state_text, sizeof(state_text), "ON");
	}
	lck_mtx_unlock(&tfa_lock);
	return ok;
}

bool
dlisa_tfa_healthy(void)
{
	bool ok = true;

	lck_mtx_lock(&tfa_lock);
	if (!on) {
		lck_mtx_unlock(&tfa_lock);
		return !broken;
	}
	for (unsigned i = 0; ok && i < AMPS; i++) {
		uint16_t c0 = 0;

		if (!wanted(&amps[i])) {
			continue;
		}
		if (!reg_read(amps[i].address, SYS_CTRL0, &c0)) {
			say("sound: tfa: %s: NO ANSWER while playing\n", amps[i].name);
			ok = false;
			break;
		}
		if ((c0 & SYS_CTRL0_WATCHED) != (SYS_CTRL0_AMPE | (dcdc ? SYS_CTRL0_DCDC : 0))) {
			say("sound: tfa: %s: SYS_CTRL0 %#06x while playing: NOT AS WRITTEN (a reset or a power-down of its own?)\n",
			    amps[i].name, c0);
			ok = false;
			break;
		}
		flags(&amps[i], "while playing");
	}
	if (!ok) {
		give_up("an amplifier is not as it was left");
	}
	lck_mtx_unlock(&tfa_lock);
	return ok;
}

void
dlisa_tfa_stop(void)
{
	lck_mtx_lock(&tfa_lock);
	if (on) {
		for (unsigned i = 0; i < AMPS; i++) {
			if (wanted(&amps[i]) && amps[i].found) {
				say("sound: tfa: %s: muted (AMPE 0)\n", amps[i].name);
				(void)put_bits(&amps[i], SYS_CTRL0, SYS_CTRL0_AMPE, 0);
			}
		}
		sleep_ms(5);
		for (unsigned i = 0; i < AMPS; i++) {
			if (wanted(&amps[i]) && amps[i].found) {
				say("sound: tfa: %s: powered down (PWDN 1)\n", amps[i].name);
				if (!put_bits(&amps[i], SYS_CTRL0, SYS_CTRL0_PWDN, SYS_CTRL0_PWDN)) {
					all_off("the power-down did not read back");
				}
				flags(&amps[i], "stopped");
			}
		}
		on = false;
		if (!broken) {
			snprintf(state_text, sizeof(state_text), "muted and powered down");
		}
	}
	lck_mtx_unlock(&tfa_lock);
}

/* No lock (whoever held it may be stopped mid-transaction), no sleep: the I2C waits are spins. */
void
dlisa_tfa_quiesce(void)
{
	if (tlmm == 0) {
		return;
	}
	for (unsigned i = 0; i < AMPS; i++) {
		uint16_t c0 = 0;

		if (amps[i].found && reg_read(amps[i].address, SYS_CTRL0, &c0)) {
			(void)reg_write(amps[i].address, SYS_CTRL0, (uint16_t)((c0 & ~SYS_CTRL0_AMPE) | SYS_CTRL0_PWDN));
		}
	}
	on = false;
}

bool
dlisa_tfa_on(void)
{
	return on;
}

const char *
dlisa_tfa_state(void)
{
	return state_text;
}
