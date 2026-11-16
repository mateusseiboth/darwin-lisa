/*
 * darwin-lisa: the vibration motor of the Xiaomi 11 Lite 5G NE (lisa): a linear resonant actuator driven by an Awinic
 * AW8624 (chip ID 0x24) at I2C address 0x5a. From the stock overlay (dtbo entry "lisa based ... SM7325", fragment on
 * qupv3_se4_i2c, node aw8624_haptic@5A) and Xiaomi's driver (MiCode Xiaomi_Kernel_OpenSource lisa-r-oss,
 * drivers/input/misc/aw8624_haptic/aw8624.c and aw8624_reg.h):
 *
 *   I2C        QUP serial engine 4 (0x990000), GPIO 16 data and 17 clock (function qup04; pmOS's qup-i2c4-data-clk);
 *              driven by hand here as HakiOS did for redwood's AW86927: the two pins taken for each transaction and
 *              given back as they were found (no GENI clocks, no serial engine firmware involved)
 *   reset      GPIO 35, high = running (Android: low 5 ms, then high)
 *   interrupt  GPIO 18 (not used: polled)
 *   effects    Android's RAM waveforms, /vendor/firmware/aw8624_haptic.bin (5.2 KB, checksum in its first two bytes),
 *              eleven of them: 1..10 the short effects (Android's 0..9), 11 the one looped for a vibration of some length
 *              (vib_effect_id_boundary 10: Android loops boundary + 1). Built into the kernel by periph-fw.py (from the
 *              stock vendor image on this PC, not in git), else fw/aw8624_haptic.bin of the image or root.
 *
 * What Android writes at probe that is kept: standby, PWM 24 kHz, the motor protection off, the battery-voltage
 * compensation in hardware, the brake (vib_sw_brake 0x2c), zero-cross threshold 0x8f8, tset 0x11, back-EMF thresholds
 * 00 08 03 f8 (lisa's tree). Not done: the f0 calibration (TRIM_LRA stays 0, the uncalibrated motor: a little weaker).
 *
 * Boot-args (nothing happens without one):
 *   haptic=1   a look, reading only: the pins
 *   haptic=2   ... the reset released (low 5 ms, high), the chip ID read; nothing written to the chip
 *   haptic=3   ... Android's configuration written (every write read back), the waveforms loaded into the RAM and
 *              checked; standby. Nothing moves
 *   haptic=4   ... and one effect played (haptic_effect=N, 1..11, default 2), haptic_secs after (default 1)
 *   haptic=5   configured and loaded as for 3, then the motor for Darwin until the run ends:
 *                /dev/hakios-haptic  write a 32-bit command: 0 stop, 1..10 a short effect, 1000 + MS a vibration of MS
 *                                    milliseconds (the looped waveform; HAPTIC_MAX_MS at most)
 *                kern.hakios.haptic_play (RW int): the same command; reads how many were played
 *                kern.hakios.haptic_status (string): state, counts, the last flags read
 *              haptic_seq=CMD@S,...  test commands, S seconds after the driver starts (e.g. haptic_seq=2@200,1400@205)
 *
 * Limits: Android's gain (0x80) at most; no vibration longer than HAPTIC_MAX_MS, then stop and standby; any fault the
 * chip reports (over-voltage, under-voltage, over-current, over-temperature), a write that does not read back or a
 * byte nobody acknowledges: the chip is put into reset and nothing more is done for the rest of the run.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern const uint8_t *dlisa_periph_file(const char *name, uint64_t *size);

static vm_offset_t tlmm;
static uint32_t level;
static bool broken, ready;

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
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* ---- pins ------------------------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 16
#define PIN_SCL                 17
#define PIN_INT                 18
#define PIN_RESET               35

static void
show_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { PIN_RESET, "reset" }, { PIN_INT, "interrupt" } };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		printf("haptic: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

static void
reset_pin(bool high)
{
	wr(GPIO_IO(PIN_RESET), high ? 2 : 0);
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
}

/* ---- I2C by hand (HakiOS hakios.haptic.c / hakios.speaker.c) --------------------------------------------------- */

static LCK_GRP_DECLARE(haptic_lock_group, "dlisa-haptic");
static LCK_MTX_DECLARE(haptic_lock, &haptic_lock_group);

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
	printf("haptic: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
	    level_of(PIN_SDA), PIN_SCL, level_of(PIN_SCL));
	return false;
}

#define AW_ADDRESS              0x5a

static bool
regs_read(uint8_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(AW_ADDRESS << 1) && i2c_write(reg);
	answered ? i2c_start() : (void)0;
	answered = answered && i2c_write(AW_ADDRESS << 1 | 1);
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_read(i + 1 < count);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static bool
regs_write(uint8_t reg, const uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(AW_ADDRESS << 1) && i2c_write(reg);
	for (unsigned i = 0; answered && i < count; i++) {
		answered = i2c_write(values[i]);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

/* ---- the chip (aw8624_reg.h) ---------------------------------------------------------------------------------- */

#define AW_ID                   0x00            /* 0x24; writing 0xaa soft-resets */
#define AW_SYSINT               0x02            /* read: cleared */
#define AW_SYSINTM              0x03            /* [5] 1 = under-voltage interrupt masked */
#define AW_SYSCTRL              0x04            /* [5] RAM init, [3:2] play mode (0 RAM), [0] 1 standby */
#define AW_GO                   0x05            /* [0] */
#define AW_WAVSEQ1              0x07
#define AW_WAVSEQ2              0x08
#define AW_WAVLOOP1             0x0f            /* [7:4] the first waveform's loops, 15 forever */
#define AW_DBGCTRL              0x20            /* [5] 0 = the INTN pin is a trigger input */
#define AW_BASE_ADDRH           0x21
#define AW_BASE_ADDRL           0x22
#define AW_FIFO_AEH             0x23
#define AW_FIFO_AEL             0x24
#define AW_FIFO_AFH             0x25
#define AW_FIFO_AFL             0x26
#define AW_PWMPRC               0x2d            /* [7] protection */
#define AW_PWMDBG               0x2e            /* [6:5] 2 = 24 kHz */
#define AW_WAVECTRL             0x31            /* [7:4] the over-drive count */
#define AW_SW_BRAKE             0x39
#define AW_DATDBG               0x3b            /* the gain */
#define AW_PRLVL                0x3e            /* [7] protection */
#define AW_RAMADDRH             0x40
#define AW_RAMADDRL             0x41
#define AW_RAMDATA              0x42
#define AW_GLB_STATE            0x47            /* [3:0] 0 standby */
#define AW_TSET                 0x4d
#define AW_THRS_BRA_END         0x4f
#define AW_TRIM_LRA             0x5b
#define AW_R_SPARE              0x5d            /* [7] Android sets it before the f0 calibration */
#define AW_DETCTRL              0x5f            /* [5] 1 = protection takes no action */
#define AW_ADCTEST              0x66            /* [6] 1 = battery compensation in hardware */
#define AW_ZC_THRSH_H           0x72
#define AW_ZC_THRSH_L           0x73
#define AW_BEMF_VTHH_H          0x74

#define AW_CHIP_ID              0x24
#define SYSINT_FAULTS           0x66            /* OVI 0x40, UVLI 0x20, OCDI 0x04, OTI 0x02 */
#define ANDROID_GAIN            0x80
#define LOOP_WAVEFORM           11
#define SHORT_EFFECTS           10
#define HAPTIC_MAX_MS           3000

static uint8_t last_flags;
static uint32_t played, faults_seen;

static void
give_up(const char *why)
{
	broken = true;
	printf("haptic: %s: the chip is put into reset (gpio%u low) and nothing more is done\n", why, PIN_RESET);
	reset_pin(false);
}

static bool
get(uint8_t reg, uint8_t *value)
{
	bool ok = regs_read(reg, value, 1);

	if (!ok) {
		printf("haptic: %#04x NOT READ\n", reg);
	}
	return ok;
}

static bool
put(uint8_t reg, uint8_t value)
{
	uint8_t after = 0;

	if (broken) {
		return false;
	}
	if (!regs_write(reg, &value, 1) || !regs_read(reg, &after, 1)) {
		give_up("a write or its read-back was not acknowledged");
		return false;
	}
	if (after != value) {
		printf("haptic: %#04x <- %#04x, reads %#04x\n", reg, value, after);
		give_up("a register did not read back as written");
		return false;
	}
	return true;
}

static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0;

	if (!get(reg, &now)) {
		give_up("a read failed");
		return false;
	}
	return (now & mask) == (value & mask) || put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

/* the interrupt flags (a read clears them): false, and the chip given up on, on a fault */
static bool
healthy(const char *when)
{
	if (!get(AW_SYSINT, &last_flags)) {
		give_up("the flags could not be read");
		return false;
	}
	if (last_flags & SYSINT_FAULTS) {
		faults_seen++;
		printf("haptic: %s: the chip reports %#04x (over-voltage 0x40, under-voltage 0x20, over-current 0x04, "
		    "over-temperature 0x02)\n", when, last_flags);
		give_up("a fault");
		return false;
	}
	return true;
}

static bool
release_and_identify(void)
{
	uint8_t id = 0;

	printf("haptic: reset released as Android does (low 5 ms, then high)\n");
	reset_pin(false);
	delay_for_interval(5, NSEC_PER_MSEC);
	reset_pin(true);
	delay_for_interval(5, NSEC_PER_MSEC);
	if (!get(AW_ID, &id)) {
		give_up("nobody answers at 0x5a");
		return false;
	}
	printf("haptic: chip ID %#04x%s\n", id, id == AW_CHIP_ID ? " (AW8624)" : " (NOT THE AW8624)");
	if (id != AW_CHIP_ID) {
		give_up("not the chip this driver knows");
		return false;
	}
	return true;
}

static bool
standby(void)
{
	return put_bits(AW_SYSINTM, 0x20, 0x20) && put_bits(AW_SYSCTRL, 0x01, 0x01) && put_bits(AW_DBGCTRL, 0x20, 0x00);
}

/* Android's aw8624_haptic_init(), less the f0 calibration */
static bool
configure(void)
{
	static const uint8_t bemf[4] = { 0x00, 0x08, 0x03, 0xf8 };
	uint8_t soft_reset = 0xaa;
	bool ok;

	(void)regs_write(AW_ID, &soft_reset, 1);        /* (no read-back: the ID register keeps 0x24) */
	delay_for_interval(2, NSEC_PER_MSEC);
	ok = standby() && put_bits(AW_PWMDBG, 0x60, 0x40);
	ok = ok && put_bits(AW_DETCTRL, 0x20, 0x20) && put_bits(AW_PWMPRC, 0x80, 0x00) && put_bits(AW_PRLVL, 0x80, 0x00);
	ok = ok && put_bits(AW_ADCTEST, 0x40, 0x40) && put(AW_TRIM_LRA, 0x00);
	ok = ok && put_bits(AW_R_SPARE, 0x80, 0x80);
	ok = ok && put(AW_SW_BRAKE, 0x2c) && put(AW_THRS_BRA_END, 0x00) && put_bits(AW_WAVECTRL, 0xf0, 0x00);
	ok = ok && put(AW_ZC_THRSH_H, 0x08) && put(AW_ZC_THRSH_L, 0xf8) && put(AW_TSET, 0x11);
	for (unsigned i = 0; ok && i < 4; i++) {
		ok = put((uint8_t)(AW_BEMF_VTHH_H + i), bemf[i]);
	}
	return ok && healthy("after the configuration");
}

/* the waveforms into the RAM (aw8624_container_update), then read back */
static bool
load_ram(void)
{
	uint64_t size = 0;
	const uint8_t *bin = dlisa_periph_file("aw8624_haptic.bin", &size);
	uint16_t sum = 0, base;
	uint8_t check[16];
	bool ok;

	if (bin == NULL || size < 8 || size > 0x4000) {
		printf("haptic: no aw8624_haptic.bin (built in by periph-fw.py, or fw/ of the image): no waveforms\n");
		return false;
	}
	for (uint64_t i = 2; i < size; i++) {
		sum = (uint16_t)(sum + bin[i]);
	}
	if (sum != (uint16_t)(bin[0] << 8 | bin[1])) {
		printf("haptic: aw8624_haptic.bin's checksum is %#06x, the file says %#06x: not loaded\n", sum, bin[0] << 8 | bin[1]);
		return false;
	}
	base = (uint16_t)(bin[2] << 8 | bin[3]);
	printf("haptic: aw8624_haptic.bin: %llu bytes, checksum %#06x, RAM base %#06x\n", size, sum, base);
	ok = put_bits(AW_SYSCTRL, 0x20, 0x20);
	ok = ok && put(AW_BASE_ADDRH, bin[2]) && put(AW_BASE_ADDRL, bin[3]);
	ok = ok && put(AW_FIFO_AEH, (uint8_t)((base >> 1) >> 8)) && put(AW_FIFO_AEL, (uint8_t)(base >> 1));
	ok = ok && put(AW_FIFO_AFH, (uint8_t)((base - (base >> 2)) >> 8)) && put(AW_FIFO_AFL, (uint8_t)(base - (base >> 2)));
	ok = ok && put(AW_RAMADDRH, bin[2]) && put(AW_RAMADDRL, bin[3]);
	for (uint64_t i = 4; ok && i < size; i += 128) {
		ok = regs_write(AW_RAMDATA, bin + i, (unsigned)MIN(128, size - i));
	}
	/* the first 16 bytes read back through the same port */
	ok = ok && put(AW_RAMADDRH, bin[2]) && put(AW_RAMADDRL, bin[3]);
	for (unsigned i = 0; ok && i < sizeof(check); i++) {
		ok = regs_read(AW_RAMDATA, &check[i], 1);       /* (one byte a transaction, as Android reads it) */
	}
	ok = ok && memcmp(check, bin + 4, MIN(16, size - 4)) == 0;
	ok = put_bits(AW_SYSCTRL, 0x20, 0x00) && ok;
	printf("haptic: waveforms %s\n", ok ? "loaded and read back" : "NOT LOADED");
	return ok;
}

static bool
stop(void)
{
	uint8_t state = 0xff;

	if (!put_bits(AW_GO, 0x01, 0x00)) {
		return false;
	}
	for (int i = 0; i < 100 && get(AW_GLB_STATE, &state) && (state & 0x0f) != 0; i++) {
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	return standby();
}

/* waveform `wave' once (loops 0) or until stopped (loops 15), at Android's gain */
static bool
start(uint8_t wave, uint8_t loops)
{
	bool ok = stop() && put(AW_WAVSEQ1, wave) && put(AW_WAVSEQ2, 0x00) && put_bits(AW_WAVLOOP1, 0xf0, (uint8_t)(loops << 4));

	ok = ok && put_bits(AW_SYSCTRL, 0x0c, 0x00) && put_bits(AW_SYSCTRL, 0x01, 0x00) && healthy("before play");
	ok = ok && put_bits(AW_SYSINTM, 0x20, 0x00) && put(AW_DATDBG, ANDROID_GAIN) && put_bits(AW_GO, 0x01, 0x01);
	played += ok;
	return ok;
}

/* one command: 0 stop, 1..10 a short effect, 1000 + ms a vibration */
static bool
run_command(uint32_t command)
{
	uint32_t ms = command >= 1000 ? MIN(command - 1000, HAPTIC_MAX_MS) : 0;
	uint64_t until = now_ms() + ms;
	bool ok;

	lck_mtx_lock(&haptic_lock);
	ok = !broken && ready;
	ok = ok && (command == 0 ? stop() : command <= SHORT_EFFECTS ? start((uint8_t)command, 0) :
	    command >= 1000 ? start(LOOP_WAVEFORM, 15) : false);
	lck_mtx_unlock(&haptic_lock);
	while (ok && ms != 0 && now_ms() < until) {
		delay_for_interval(20, NSEC_PER_MSEC);
		lck_mtx_lock(&haptic_lock);
		ok = healthy("while vibrating");
		lck_mtx_unlock(&haptic_lock);
	}
	lck_mtx_lock(&haptic_lock);
	ok = ms != 0 && !broken ? stop() && ok : ok;
	lck_mtx_unlock(&haptic_lock);
	printf("haptic: command %u %s\n", command, ok ? "played" : "FAILED");
	return ok;
}

/* ---- Darwin's side: /dev/hakios-haptic, kern.hakios.haptic_* ------------------------------------------------- */

static void
command_thread(void *parameter, __unused wait_result_t result)
{
	run_command((uint32_t)(uintptr_t)parameter);
	thread_terminate(current_thread());
}

static int
post_command(uint32_t command)
{
	thread_t thread;

	if (level < 5 || !ready || broken) {
		return ENODEV;
	}
	if (command > SHORT_EFFECTS && command < 1000) {
		return EINVAL;
	}
	if (kernel_thread_start(command_thread, (void *)(uintptr_t)command, &thread) != KERN_SUCCESS) {
		return ENOMEM;
	}
	thread_deallocate(thread);
	return 0;
}

static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t command = 0;
	int error = 0;

	while (error == 0 && uio_resid(uio) >= (user_ssize_t)sizeof(command)) {
		error = uiomove((caddr_t)&command, sizeof(command), uio);
		error = error != 0 ? error : post_command(command);
	}
	return error;
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

SYSCTL_DECL(_kern_hakios);

static int
sysctl_haptic_play SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = (int)played, error = SYSCTL_OUT(req, &value, sizeof(value));

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	error = SYSCTL_IN(req, &value, sizeof(value));
	return error != 0 ? error : value < 0 ? EINVAL : post_command((uint32_t)value);
}

static int
sysctl_haptic_status SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[120];

	snprintf(text, sizeof(text), "level %u ready %u broken %u played %u faults %u flags %#x", level, ready, broken, played,
	    faults_seen, last_flags);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, haptic_play, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    sysctl_haptic_play, "I", "write 0 stop, 1..10 an effect, 1000+ms a vibration (haptic=5); reads how many were played");
SYSCTL_PROC(_kern_hakios, OID_AUTO, haptic_status, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctl_haptic_status, "A", "the vibration motor driver's state");

/* ---- the start ---------------------------------------------------------------------------------------------- */

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

static void
run_sequence(void)
{
	const char *cursor = seq_text;
	uint64_t start_ms = now_ms();

	for (int n = 0; n < SEQ_MAX && *cursor != 0; n++) {
		const char *at = strchr(cursor, '@'), *end = strchr(cursor, ',');
		uint64_t command = parse_decimal(cursor), when = at != NULL ? parse_decimal(at + 1) * 1000 : 0;

		if (at == NULL) {
			break;
		}
		while (now_ms() < start_ms + when) {
			delay_for_interval(100, NSEC_PER_MSEC);
		}
		printf("haptic: haptic_seq: command %llu\n", command);
		run_command((uint32_t)command);
		cursor = end != NULL ? end + 1 : "";
	}
}

static void
serve(void)
{
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		printf("haptic: /dev/hakios-haptic cannot be made\n");
	}
	printf("haptic: the motor is Darwin's (/dev/hakios-haptic, kern.hakios.haptic_play)\n");
	if (PE_parse_boot_arg_str("haptic_seq", seq_text, sizeof(seq_text))) {
		run_sequence();
	}
}

static void
play_once(void)
{
	uint32_t effect = 2, secs = 1;

	PE_parse_boot_argn("haptic_effect", &effect, sizeof(effect));
	PE_parse_boot_argn("haptic_secs", &secs, sizeof(secs));
	effect = effect < 1 || effect > LOOP_WAVEFORM ? 2 : effect;
	delay_for_interval(MIN(secs, 600), NSEC_PER_SEC);
	printf("haptic: effect %u, once\n", effect);
	run_command(effect == LOOP_WAVEFORM ? 1500 : effect);
}

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	show_pins();
	lck_mtx_lock(&haptic_lock);
	ready = level >= 2 && release_and_identify() && level >= 3 && configure() && load_ram() && healthy("after loading");
	lck_mtx_unlock(&haptic_lock);
	printf("haptic: %s\n", ready ? "configured, waveforms loaded, standby" : level >= 3 ? "NOT READY" : "look done");
	ready && level == 4 ? play_once() : (void)0;
	ready && level == 5 ? serve() : (void)0;
	thread_terminate(current_thread());
}

void dlisa_haptic_init(void);

void
dlisa_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = MIN(level, 5);
	printf("haptic: level %u (AW8624 at 0x5a on gpio%u/%u, reset gpio%u)\n", level, PIN_SDA, PIN_SCL, PIN_RESET);
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
