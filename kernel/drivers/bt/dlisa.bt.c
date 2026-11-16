/*
 * darwin-lisa: Bluetooth of the WCN6750 on the Xiaomi 11 Lite 5G NE (lisa, SM7325), over the UART of QUP0 serial
 * engine 7, polled (this kernel has no interrupts for it).
 *
 * Hardware (pmOS DTB of lisa, Linux 7.2.9 drivers/{tty/serial/qcom_geni_serial.c, soc/qcom/qcom-geni-se.c,
 * clk/qcom/{gcc-sc7280.c,clk-rcg2.c,clk-alpha-pll.c}, bluetooth/{hci_qca.c,btqca.c}, regulator/qcom-rpmh-regulator.c},
 * Xiaomi's lisa btpower.c):
 *   UART       uart7 = QUP0 SE7 at 0x99c000, wrapper at 0x9c0000; gpio28 CTS, 29 RTS, 30 TX, 31 RX, function qup07 (1)
 *   enable     gpio85 BT_EN (high = on); gpio86 SW_CTRL (input, the chip raises it once up)
 *   clocks     GCC vote 0x52008: 6 wrap0 m_ahb, 7 s_ahb, 8 core, 9 core_2x, 17 s7; RCG wrap0_s7 at 0x17860
 *   supplies   l19b 1.8 V (vddio), s7b 0.952 V (aon, cxmx, rfacmn, rfa0p8), s8b 1.256 V (rfa1p2), s1b 1.856 V (rfa1p7),
 *              s1c 2.2 V (rfa2p2), l11c 2.8 V (asd); s7b/s8b/s1b are Wi-Fi's too. PM7325 = SPMI slave 1 ("b"),
 *              PM8350C = slave 2 ("c"); Sn's control at 0x1400 + 0x300 (n - 1), Ln's at 0x4000 + 0x100 (n - 1)
 *   firmware   msbtfw11.mbn (else msbtfw11.tlv) and msnv11.bin through dlisa_periph_file()
 *
 * Boot-args (nothing at all without bt=):
 *   bt=1   a look, reads only: pins, clocks, PLL, supplies; the serial engine's registers only if its clocks are on
 *   bt=2   ... supplies (voted through RPMh only if a read says off), clocks, pins, GENI UART 115200 8N1 RTS/CTS in FIFO
 *          mode, BT_EN cycled as hci_qca does, the QCA version (EDL 0x19), then HCI reset and Read Local Version (ROM)
 *   bt=3   ... (without the ROM reset) Linux's qca_setup for QCA_WCN6750: 3 Mbit/s, patch config, rampatch, NVM,
 *          logging off, HCI reset, build info, BD_ADDR, local version; an LE scan (bt_scan_secs, default 10) and a
 *          classic inquiry (bt_inquiry=0 skips it), every device found logged
 *   bt=4   ... and the link for user space: /dev/hakios-bt (raw H4 both ways; /dev/uart.bluetooth is the same device),
 *          the kern_ctl "com.apple.uart.bluetooth" (bluetoothd's H4 POSIX transport), kern.hakios.bt_status,
 *          kern.hakios.bt_scan (write N: an LE scan of N seconds now)
 *   bt_ibs=0      do not turn on the chip's in-band sleep in the NVM (Linux does: HCI tag byte 0 | 0x80)
 *   bt_fast=0     stay at 115200 (no baud change)
 *
 * Safety: each step is logged before it is done; an unexpected answer stops the bring-up there (no next level); every
 * wait has a timeout. On giving up: BT_EN low, the engine's commands cancelled, the UART pins, clock votes, branch
 * registers and the RCG put back as they were found. Supplies this driver switched on are left on (said so). GPIO 84
 * (Wi-Fi's enable) is never read or written.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/dlisa.spmi.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kern_control_private.h>
#include <sys/kpi_mbuf.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/uio.h>
#include <sys/vnode.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern const uint8_t *dlisa_periph_file(const char *name, uint64_t *size);
extern lck_mtx_t hakios_rsc_lock;

void dlisa_bt_init(void);

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

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* ---- state ------------------------------------------------------------------------------------------------- */

static LCK_GRP_DECLARE(bt_lock_group, "dlisa-bt");
static LCK_MTX_DECLARE(bt_lock, &bt_lock_group);         /* the engine's registers, the parser, the queues */
static LCK_MTX_DECLARE(bt_cmd_lock, &bt_lock_group);     /* one kernel command sequence at a time */
static LCK_MTX_DECLARE(bridge_lock, &bt_lock_group);     /* the kern_ctl client's partial packet */

static uint32_t level, step_no;
static const char *state = "off";
static char failed_at[96];
static bool serving, kernel_owns, link_broken, scan_running;
static uint32_t baud, ibs_wanted = 1, fast_wanted = 1, inquiry_wanted = 1, scan_secs = 10;

struct chip_version {
	uint32_t product, soc;
	uint16_t patch, rom;
	uint8_t rom_code;
	bool known;
};
static struct chip_version chip;
static char fw_text[48], build_label[64], bdaddr_text[24] = "?";
static uint32_t packets_in, packets_out, errors, dropped, scans_done, scan_found;

static void
step(const char *what)
{
	printf("bt: %u: %s\n", ++step_no, what);
}

/* Ends the bring-up at this step: the reason is kept for kern.hakios.bt_status. */
static bool
fail(const char *why)
{
	printf("bt: %u: FAILED: %s\n", step_no, why);
	snprintf(failed_at, sizeof(failed_at), "step %u: %s", step_no, why);
	return false;
}

/* ---- pins (TLMM) -------------------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_KEEPER             2
#define PULL_UP                 3
#define CFG_OUTPUT              (1u << 9)
#define FUNC_QUP07              1

#define PIN_CTS                 28
#define PIN_RTS                 29
#define PIN_TX                  30
#define PIN_RX                  31
#define PIN_BT_EN               85
#define PIN_SW_CTRL             86

static vm_offset_t tlmm;

static const struct {
	unsigned pin;
	unsigned pull;          /* sc7280-idp's choice for each (lisa's tree only names the function) */
	const char *what;
} uart_pins[] = {
	{ PIN_CTS, PULL_KEEPER, "UART CTS" }, { PIN_RTS, PULL_NONE, "UART RTS" },
	{ PIN_TX, PULL_NONE, "UART TX" }, { PIN_RX, PULL_UP, "UART RX" },
};
#define UART_PINS (sizeof(uart_pins) / sizeof(uart_pins[0]))

static uint32_t found_pin_cfg[UART_PINS], found_pin_io[UART_PINS], found_en_cfg, found_en_io;
static bool pins_taken, en_driven;

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	printf("bt: gpio%u (%s): cfg %#x: function %u, pull %u, %u mA, %s; reads %u\n", pin, what, cfg, (cfg >> 2) & 15,
	    cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? ((io & 2) ? "drives high" : "drives low") : "input",
	    io & 1);
}

static void
show_pins(void)
{
	for (unsigned i = 0; i < UART_PINS; i++) {
		show_pin(uart_pins[i].pin, uart_pins[i].what);
	}
	show_pin(PIN_BT_EN, "BT_EN");
	show_pin(PIN_SW_CTRL, "SW_CTRL");
}

static void
bt_en(bool high)
{
	wr(GPIO_IO(PIN_BT_EN), high ? 2 : 0);   /* the level first, then the output enable: never the wrong level */
	wr(GPIO_CFG(PIN_BT_EN), CFG_OUTPUT | PULL_NONE);
}

static bool
take_uart_pins(void)
{
	step("the UART pins (gpio28-31) to function qup07, 2 mA; BT_EN (gpio85) taken as a GPIO output");
	for (unsigned i = 0; i < UART_PINS; i++) {
		found_pin_cfg[i] = rd(GPIO_CFG(uart_pins[i].pin));
		found_pin_io[i] = rd(GPIO_IO(uart_pins[i].pin));
	}
	found_en_cfg = rd(GPIO_CFG(PIN_BT_EN));
	found_en_io = rd(GPIO_IO(PIN_BT_EN));
	pins_taken = true;
	printf("bt: BT_EN found as cfg %#x io %#x (driven from the power-up on; low again if the bring-up gives up)\n",
	    found_en_cfg, found_en_io);
	for (unsigned i = 0; i < UART_PINS; i++) {
		wr(GPIO_CFG(uart_pins[i].pin), uart_pins[i].pull | FUNC_QUP07 << 2);
	}
	show_pins();
	return true;
}

static void
give_back_pins(void)
{
	if (!pins_taken) {
		return;
	}
	for (unsigned i = 0; i < UART_PINS; i++) {
		wr(GPIO_IO(uart_pins[i].pin), found_pin_io[i] & 2);
		wr(GPIO_CFG(uart_pins[i].pin), found_pin_cfg[i]);
	}
	printf("bt: the UART pins are back as found (cfg %#x %#x %#x %#x)\n", found_pin_cfg[0], found_pin_cfg[1],
	    found_pin_cfg[2], found_pin_cfg[3]);
}

/* ---- supplies: their state over SPMI, a vote through RPMh when off ---------------------------------------------- */

struct supply {
	const char *rail, *rpmh, *uses;
	uint16_t ppid;          /* (slave << 8) | peripheral of the regulator's control */
	uint32_t mv;
	bool ldo;
};
static const struct supply supplies[] = {       /* in hci_qca's enable order */
	{ "l19b", "ldob19", "vddio", 0x152, 1800, true },
	{ "s7b", "smpb7", "vddaon/btcxmx/rfacmn/rfa0p8", 0x126, 952, false },
	{ "s1b", "smpb1", "vddrfa1p7", 0x114, 1856, false },
	{ "s8b", "smpb8", "vddrfa1p2", 0x129, 1256, false },
	{ "s1c", "smpc1", "vddrfa2p2", 0x214, 2200, false },
	{ "l11c", "ldoc11", "vddasd", 0x24a, 2800, true },
};
#define SUPPLIES (sizeof(supplies) / sizeof(supplies[0]))

enum rail_state { RAIL_UNKNOWN, RAIL_OFF, RAIL_ON };
static bool rail_voted[SUPPLIES];

/* qcom_spmi-regulator: 0x04/0x05 type/subtype, 0x08 status (bit 7 in regulation), 0x40/0x41 mV, 0x46 bit 7 enable */
static enum rail_state
read_rail(const struct supply *s, bool quiet)
{
	struct dlisa_spmi_peripheral p;
	uint8_t type[2] = { 0 }, status = 0, mv[2] = { 0 }, enable = 0;
	const char *problem = dlisa_spmi_find(s->ppid, &p);

	problem = problem ? problem : dlisa_spmi_read(&p, 0x04, type, 2);
	problem = problem ? problem : dlisa_spmi_read(&p, 0x08, &status, 1);
	problem = problem ? problem : dlisa_spmi_read(&p, 0x40, mv, 2);
	problem = problem ? problem : dlisa_spmi_read(&p, 0x46, &enable, 1);
	if (problem != NULL) {
		printf("bt: supply %s (%s, SPMI %#x): not read: %s\n", s->rail, s->uses, s->ppid, problem);
		return RAIL_UNKNOWN;
	}
	if (type[0] != 0x03 && type[0] != 0x04 && type[0] != 0x1c && type[0] != 0x21 && type[0] != 0x22) {
		printf("bt: supply %s (SPMI %#x): type %#x/%#x is not a regulator's: state unknown\n", s->rail, s->ppid, type[0],
		    type[1]);
		return RAIL_UNKNOWN;
	}
	if (!quiet) {
		printf("bt: supply %s (%s, SPMI %#x, type %#x/%#x): status %#x (%s), enable %#x, set %u mV\n", s->rail, s->uses,
		    s->ppid, type[0], type[1], status, (status & 0x80) ? "in regulation" : "NOT in regulation", enable,
		    (unsigned)(mv[0] | mv[1] << 8));
	}
	return (status & 0x80) ? RAIL_ON : RAIL_OFF;
}

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's DRV 2 */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* TCS 0, an "active" one (goodix, adsp, mpss share it) */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

static vm_offset_t rsc, cmd_db;

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* the RPMh address of a resource in the boot loader's command database (hakios.goodix.c); 0 = none */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(const void *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
tcs_wait(uint32_t reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return false;
}

/* up to three writes to RPMh, as hakios.goodix.c's rpmh_write (the same slots, under hakios_rsc_lock) */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done;

	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	if (!tcs_wait(TCS_STATUS, 1, 1)) {
		lck_mtx_unlock(&hakios_rsc_lock);
		printf("bt: RPMh: the command slots stay busy\n");
		return false;
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = tcs_wait(TCS_IRQ_STATUS, 1, 1);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

/* voltage, enable (and full-power mode for an LDO, the tree's regulator-initial-mode 3) */
static bool
rpmh_rail_on(const struct supply *s)
{
	uint32_t base = rpmh_address(s->rpmh);
	uint32_t address[3] = { base, base + 4, base + 8 }, data[3] = { s->mv, 1, 7 };

	if (base == 0 || ((base >> 16) & 0xf) != 4) {
		printf("bt: RPMh has no regulator \"%s\" (address %#x)\n", s->rpmh, base);
		return false;
	}
	printf("bt: RPMh: %s (%#x) <- %u mV, enable%s\n", s->rpmh, base, s->mv, s->ldo ? ", high-power mode" : "");
	return rpmh_write(address, data, s->ldo ? 3 : 2);
}

static void
show_supplies(void)
{
	for (unsigned i = 0; i < SUPPLIES; i++) {
		(void)read_rail(&supplies[i], false);
	}
}

static bool
rail_comes_up(const struct supply *s)
{
	for (int i = 0; i < 50; i++) {
		if (read_rail(s, true) == RAIL_ON) {
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

static bool
supplies_on(void)
{
	step("the supplies: read each over SPMI; a vote through RPMh only for one that reads off");
	for (unsigned i = 0; i < SUPPLIES; i++) {
		const struct supply *s = &supplies[i];
		enum rail_state now = read_rail(s, false);

		if (now == RAIL_ON) {
			continue;
		}
		if (now == RAIL_UNKNOWN) {
			printf("bt: supply %s: state unknown: not voted (the chip will not answer if it is really off)\n", s->rail);
			continue;
		}
		if (cmd_db == 0) {
			cmd_db = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
			rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		}
		if (!rpmh_rail_on(s)) {
			return fail("an RPMh vote for a supply did not complete");
		}
		rail_voted[i] = true;
		if (!rail_comes_up(s)) {
			(void)read_rail(s, false);
			return fail("a supply voted on is still not in regulation after 50 ms");
		}
		printf("bt: supply %s is up\n", s->rail);
	}
	return true;
}

/* ---- clocks (GCC) ------------------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GPLL0_MODE              (gcc + 0x0)     /* [31] lock, [30] active, [0] output */
#define GPLL0_L                 (gcc + 0x4)
#define GPLL0_USER_CTL          (gcc + 0xc)     /* [11:8] the even output's divider, [1] even output enable */
#define GPLL0_OPMODE            (gcc + 0x38)    /* 1 = run */
#define GPLL0_ALPHA             (gcc + 0x40)
#define GPLL_VOTE               (gcc + 0x52010)
#define GCC_VOTE                (gcc + 0x52008)
#define VOTE_M_AHB              (1u << 6)
#define VOTE_S_AHB              (1u << 7)
#define VOTE_CORE               (1u << 8)
#define VOTE_CORE_2X            (1u << 9)
#define VOTE_S7                 (1u << 17)
#define VOTES_ALL               (VOTE_M_AHB | VOTE_S_AHB | VOTE_CORE | VOTE_CORE_2X | VOTE_S7)
#define CBCR_M_AHB              (gcc + 0x17004)
#define CBCR_S_AHB              (gcc + 0x17008)
#define CBCR_CORE               (gcc + 0x23000)
#define CBCR_CORE_2X            (gcc + 0x23008)
#define CBCR_S7                 (gcc + 0x1785c)
#define CBCR_OFF                (1u << 31)
#define CBCR_ENABLE             (1u << 0)
#define CBCR_HWCG               (1u << 1)
#define RCG                     (gcc + 0x17860)
#define RCG_CMD                 (RCG + 0x0)     /* [0] update, [31] root off */
#define RCG_CFG                 (RCG + 0x4)     /* [4:0] 2 * divider - 1, [10:8] source, [13:12] mode, [20] hw control */
#define RCG_M                   (RCG + 0x8)
#define RCG_N                   (RCG + 0xc)     /* ~(n - m) */
#define RCG_D                   (RCG + 0x10)    /* ~2d */
#define RCG_DFSR                (RCG + 0x14)    /* [0] the engine picks a preset level */
#define RCG_PERF(l)             (RCG + 0x1c + 4 * (l))
#define RCG_PERF_M(l)           (RCG + 0x5c + 4 * (l))
#define RCG_PERF_N(l)           (RCG + 0x9c + 4 * (l))
#define XO_HZ                   19200000ULL

static vm_offset_t gcc;
static uint32_t found_vote, found_cbcr_m, found_cbcr_s, found_rcg[4];
static bool clocks_taken, rcg_changed;
static uint64_t se_hz;          /* the engine's clock */
static uint32_t clk_sel;        /* GENI_CLK_SEL: the preset level, when DFS */

static uint64_t
rcg_hz(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint64_t parent[8] = { XO_HZ, 600000000ULL, 0, 0, 0, 0, 300000000ULL, 0 };
	uint64_t hz = parent[(cfg >> 8) & 7];
	uint32_t divider = cfg & 0x1f, n;

	hz = divider ? hz * 2 / (divider + 1) : hz;
	n = ((~not_n_minus_m) & 0xffff) + (m & 0xffff);
	return ((cfg >> 12) & 3) && n ? hz * (m & 0xffff) / n : hz;
}

static bool
gpll0_running(bool quiet)
{
	uint32_t mode = rd(GPLL0_MODE), user = rd(GPLL0_USER_CTL), opmode = rd(GPLL0_OPMODE), l = rd(GPLL0_L) & 0xffff;
	uint64_t hz = XO_HZ * l + ((XO_HZ * (rd(GPLL0_ALPHA) & 0xffff)) >> 16);
	bool locked = (mode & (1u << 31)) && ((mode & (1u << 30)) || (opmode & 1)), even = (user & 2) && ((user >> 8) & 0xf) == 1;

	if (!quiet || !locked || !even) {
		printf("bt: GPLL0: mode %#x opmode %#x vote %#x L %u -> %llu Hz (%s); user_ctl %#x: even output %s, divider code %u\n",
		    mode, opmode, rd(GPLL_VOTE), l, hz, locked ? "locked, running" : "NOT RUNNING", user, (user & 2) ? "on" : "OFF",
		    (user >> 8) & 0xf);
	}
	return locked && even && hz > 590000000ULL && hz < 610000000ULL;
}

static void
show_rcg(void)
{
	uint32_t cfg = rd(RCG_CFG);

	printf("bt: wrap0_s7 RCG: cmd %#x cfg %#x m %#x n %#x d %#x dfsr %#x -> %llu Hz\n", rd(RCG_CMD), cfg, rd(RCG_M),
	    rd(RCG_N), rd(RCG_D), rd(RCG_DFSR), rcg_hz(cfg, rd(RCG_M), rd(RCG_N)));
	for (uint32_t l = 0; (rd(RCG_DFSR) & 1) && l < 8; l++) {
		printf("bt: wrap0_s7 preset level %u: %llu Hz\n", l, rcg_hz(rd(RCG_PERF(l)), rd(RCG_PERF_M(l)), rd(RCG_PERF_N(l))));
	}
}

static void
show_clocks(void)
{
	uint32_t vote = rd(GCC_VOTE);

	printf("bt: GCC vote %#x: m_ahb %u s_ahb %u core %u core_2x %u s7 %u\n", vote, !!(vote & VOTE_M_AHB),
	    !!(vote & VOTE_S_AHB), !!(vote & VOTE_CORE), !!(vote & VOTE_CORE_2X), !!(vote & VOTE_S7));
	printf("bt: CBCR m_ahb %#x s_ahb %#x core %#x core_2x %#x s7 %#x (bit 31 = off)\n", rd(CBCR_M_AHB), rd(CBCR_S_AHB),
	    rd(CBCR_CORE), rd(CBCR_CORE_2X), rd(CBCR_S7));
	show_rcg();
	(void)gpll0_running(false);
}

/* the engine's registers answer only with these on: its own clock, the cores, the AHB ones enabled */
static bool
se_clocks_on(void)
{
	uint32_t vote = rd(GCC_VOTE);
	bool ahb = ((vote & VOTE_M_AHB) || (rd(CBCR_M_AHB) & CBCR_ENABLE)) && ((vote & VOTE_S_AHB) || (rd(CBCR_S_AHB) & CBCR_ENABLE));

	return ahb && !(rd(CBCR_S7) & CBCR_OFF) && !(rd(CBCR_CORE) & CBCR_OFF) && !(rd(CBCR_CORE_2X) & CBCR_OFF);
}

static const struct {
	uint64_t hz;
	uint32_t m, n;
} se_rates[] = { { 48000000ULL, 4, 25 }, { 96000000ULL, 8, 25 } };     /* GPLL0 even (300 MHz) * m / n, gcc-sc7280 */

/* DFS: the preset level whose rate is exactly `hz` */
static bool
pick_dfs_level(uint64_t hz)
{
	for (uint32_t l = 0; l < 8; l++) {
		if (rcg_hz(rd(RCG_PERF(l)), rd(RCG_PERF_M(l)), rd(RCG_PERF_N(l))) == hz) {
			clk_sel = l;
			se_hz = hz;
			printf("bt: DFS: preset level %u is %llu Hz\n", l, hz);
			return true;
		}
	}
	return false;
}

/* clk-rcg2.c __clk_rcg2_configure_mnd + update_config, for GPLL0 even * m / n */
static bool
program_rcg(uint64_t hz, uint32_t m, uint32_t n)
{
	uint32_t cfg = rd(RCG_CFG);

	if (!rcg_changed) {             /* what was found, kept once for giving it back */
		found_rcg[0] = cfg;
		found_rcg[1] = rd(RCG_M);
		found_rcg[2] = rd(RCG_N);
		found_rcg[3] = rd(RCG_D);
		rcg_changed = true;
	}
	wr(RCG_M, (rd(RCG_M) & ~0xffffu) | m);
	wr(RCG_N, (rd(RCG_N) & ~0xffffu) | ((~(n - m)) & 0xffff));
	wr(RCG_D, (rd(RCG_D) & ~0xffffu) | ((~n) & 0xffff));
	cfg &= ~(0x1fu | 7u << 8 | 3u << 12 | 1u << 20);
	wr(RCG_CFG, cfg | 1 | 6u << 8 | 2u << 12);
	wr(RCG_CMD, rd(RCG_CMD) | 1);
	for (int i = 0; i < 500 && (rd(RCG_CMD) & 1); i++) {
		sleep_us(10);
	}
	if (rd(RCG_CMD) & 1) {
		return fail("the RCG does not take its new configuration");
	}
	se_hz = rcg_hz(rd(RCG_CFG), rd(RCG_M), rd(RCG_N));
	clk_sel = 0;
	printf("bt: wrap0_s7 RCG set to GPLL0 even * %u / %u = %llu Hz\n", m, n, se_hz);
	return se_hz == hz ? true : fail("the RCG reads back another rate");
}

static bool
set_se_rate(uint64_t hz)
{
	for (unsigned i = 0; i < sizeof(se_rates) / sizeof(se_rates[0]); i++) {
		if (se_rates[i].hz != hz) {
			continue;
		}
		if (rd(RCG_DFSR) & 1) {
			return pick_dfs_level(hz) ? true : fail("DFS is on and no preset level has the rate needed");
		}
		return program_rcg(hz, se_rates[i].m, se_rates[i].n);
	}
	return fail("no table entry for the engine's rate");
}

static bool
wait_branch(vm_offset_t cbcr, const char *name)
{
	for (int i = 0; i < 1000; i++) {
		if (!(rd(cbcr) & CBCR_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("bt: the %s branch stays off (CBCR %#x)\n", name, rd(cbcr));
	return false;
}

static bool
clocks_on(uint64_t hz)
{
	step("the clocks: GPLL0 checked (read only), the s7 RCG to the engine's rate, then the votes and branches");
	if (!gpll0_running(false)) {
		return fail("GPLL0 or its even output is not running (PLLs are not configured here)");
	}
	found_vote = rd(GCC_VOTE);
	found_cbcr_m = rd(CBCR_M_AHB);
	found_cbcr_s = rd(CBCR_S_AHB);
	clocks_taken = true;
	if (!set_se_rate(hz)) {
		return false;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTES_ALL);
	/* as hakios.goodix.c on lisa's wrap1: the vote alone may leave an AHB branch off; free-run them (HWCG off) */
	wr(CBCR_M_AHB, (rd(CBCR_M_AHB) & ~CBCR_HWCG) | CBCR_ENABLE);
	wr(CBCR_S_AHB, (rd(CBCR_S_AHB) & ~CBCR_HWCG) | CBCR_ENABLE);
	if (!wait_branch(CBCR_M_AHB, "m_ahb") || !wait_branch(CBCR_S_AHB, "s_ahb") || !wait_branch(CBCR_CORE, "core") ||
	    !wait_branch(CBCR_CORE_2X, "core_2x") || !wait_branch(CBCR_S7, "s7")) {
		show_clocks();
		return fail("a clock branch did not come on");
	}
	show_clocks();
	return true;
}

static void
give_back_clocks(void)
{
	if (!clocks_taken) {
		return;
	}
	wr(GCC_VOTE, (rd(GCC_VOTE) & ~VOTES_ALL) | (found_vote & VOTES_ALL));
	wr(CBCR_M_AHB, found_cbcr_m);
	wr(CBCR_S_AHB, found_cbcr_s);
	if (rcg_changed) {
		wr(RCG_M, found_rcg[1]);
		wr(RCG_N, found_rcg[2]);
		wr(RCG_D, found_rcg[3]);
		wr(RCG_CFG, found_rcg[0]);
		wr(RCG_CMD, rd(RCG_CMD) | 1);
	}
	printf("bt: clock votes, AHB branches%s back as found (vote %#x)\n", rcg_changed ? " and the RCG" : "", rd(GCC_VOTE));
}

/* ---- the serial engine as a UART (GENI, FIFO mode, polled) --------------------------------------------------- */

#define QUP_PHYS                0x009c0000UL
#define QUP_HW_VER              (qup + 0x4)
#define SE_PHYS                 0x0099c000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* [0] main command active, [12] secondary active */
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_SER_S_CLK_CFG      (se + 0x04c)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 2 = UART */
#define GENI_CLK_SEL            (se + 0x07c)
#define UART_LOOPBACK_CFG       (se + 0x22c)
#define UART_IO_MACRO_CTRL      (se + 0x240)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define UART_TX_TRANS_CFG       (se + 0x25c)    /* [1] 1 = CTS ignored */
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define UART_TX_WORD_LEN        (se + 0x268)
#define UART_TX_STOP_BIT_LEN    (se + 0x26c)
#define UART_TX_TRANS_LEN       (se + 0x270)
#define UART_RX_TRANS_CFG       (se + 0x280)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define UART_RX_WORD_LEN        (se + 0x28c)
#define UART_RX_STALE_CNT       (se + 0x294)
#define UART_TX_PARITY_CFG      (se + 0x2a4)
#define UART_RX_PARITY_CFG      (se + 0x2a8)
#define UART_MANUAL_RFR         (se + 0x2ac)    /* [31] manual, [1] not ready */
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)    /* [2] cancel, [1] abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_CMD0             (se + 0x630)
#define GENI_S_CMD_CTRL         (se + 0x634)
#define GENI_S_IRQ_STATUS       (se + 0x640)
#define GENI_S_IRQ_EN           (se + 0x644)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* [31] last word partial, [30:28] its bytes, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define GENI_IOS                (se + 0x908)    /* [1] CTS in */
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] TX FIFO depth (words) */
#define SE_HW_PARAM_1           (se + 0xe28)    /* [21:16] RX FIFO depth */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_COMMON_IRQ            (0x7eu | 1u << 22 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 28 | 1u << 29)
#define M_RX_IRQS               (1u << 26 | 1u << 27)
#define S_CMD_CANCEL            (1u << 4)
#define S_PARITY                (1u << 9)
#define S_FRAMING               (1u << 10)
#define S_BREAK                 (3u << 11)
#define S_RX_OVERRUN            (1u << 25)
#define S_COMMON_IRQ            (0x3eu | 0x3e00u | 1u << 24 | 1u << 25)
#define S_RX_IRQS               (1u << 26 | 1u << 27)
#define UART_START_TX           (1u << 27)
#define UART_START_READ         (1u << 27)
#define PROTO_UART              2
#define QUP_SE_VERSION_2_5      0x20050000u
#define FAST_BAUD               3000000

static vm_offset_t qup, se;
static uint32_t tx_depth, rx_depth, sampling = 16;
static bool se_ready;

static void
show_se(void)
{
	uint32_t fw = rd(GENI_FW_REVISION_RO);

	printf("bt: SE7: fw revision %#x (protocol %u%s), interface disabled %#x, hw_param %#x %#x, status %#x, clk_cfg %#x, "
	    "clk_sel %#x, dma %#x, gsi %#x; QUP version %#x\n", fw, (fw >> 8) & 0xff, ((fw >> 8) & 0xff) == PROTO_UART ? " = UART" :
	    " NOT UART", rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), rd(SE_HW_PARAM_1), rd(GENI_STATUS), rd(GENI_SER_M_CLK_CFG),
	    rd(GENI_CLK_SEL), rd(GENI_DMA_MODE_EN), rd(SE_GSI_EVENT_EN), rd(QUP_HW_VER));
}

static bool
wait_bits(vm_offset_t reg, uint32_t mask, uint32_t value, uint32_t us)
{
	for (uint32_t i = 0; i < us / 10 + 1; i++) {
		if ((rd(reg) & mask) == value) {
			return true;
		}
		sleep_us(10);
	}
	return false;
}

static void
cancel_tx(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	if (!wait_bits(GENI_M_IRQ_STATUS, M_CMD_CANCEL, M_CMD_CANCEL, 20000)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		(void)wait_bits(GENI_M_IRQ_STATUS, M_CMD_ABORT, M_CMD_ABORT, 20000);
	}
	wr(GENI_M_IRQ_CLEAR, M_CMD_CANCEL | M_CMD_ABORT | M_CMD_DONE);
}

static void
stop_rx(void)
{
	if (!(rd(GENI_STATUS) & (1u << 12))) {
		return;
	}
	wr(GENI_S_CMD_CTRL, 1u << 2);
	(void)wait_bits(GENI_S_IRQ_STATUS, S_CMD_CANCEL, S_CMD_CANCEL, 20000);
	wr(GENI_S_IRQ_CLEAR, rd(GENI_S_IRQ_STATUS));
	if (rd(GENI_STATUS) & (1u << 12)) {
		wr(GENI_S_CMD_CTRL, 1u << 1);
		(void)wait_bits(GENI_S_CMD_CTRL, 1u << 1, 0, 20000);
		wr(GENI_S_IRQ_CLEAR, 0xffffffffu);
		wr(GENI_FORCE_DEFAULT, 1);
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
}

static void
start_rx(void)
{
	wr(GENI_S_CMD0, (rd(GENI_S_CMD0) & ~0xffffffffu) | UART_START_READ);
	wr(GENI_S_IRQ_EN, rd(GENI_S_IRQ_EN) | S_RX_IRQS);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | M_RX_IRQS);
}

/* qcom_geni_serial.c geni_serial_set_rate: the engine's clock / (baud * sampling), which must be (nearly) whole */
static bool
set_baud(uint32_t new_baud)
{
	uint64_t per_bit = (uint64_t)new_baud * sampling, div = (se_hz + per_bit / 2) / per_bit, actual;

	if (div == 0 || div > 0xfff) {
		return fail("no clock divider for that baud rate");
	}
	actual = se_hz / (div * sampling);
	if ((actual > new_baud ? actual - new_baud : new_baud - actual) * 100 > new_baud) {
		return fail("the baud rate would be off by more than 1%");
	}
	wr(GENI_SER_M_CLK_CFG, (uint32_t)div << 4 | 1);
	wr(GENI_SER_S_CLK_CFG, (uint32_t)div << 4 | 1);
	wr(GENI_CLK_SEL, clk_sel & 7);
	baud = new_baud;
	printf("bt: UART at %u baud: %llu Hz / (%llu * %u) = %llu baud\n", new_baud, se_hz, div, sampling, actual);
	return true;
}

/* flow control as hci_uart_set_flow_control: stopped = RTS held not-ready by hand and CTS ignored */
static void
flow_stopped(bool stop)
{
	uint32_t tx_cfg = rd(UART_TX_TRANS_CFG);

	wr(UART_TX_TRANS_CFG, stop ? tx_cfg | 2 : tx_cfg & ~2u);
	wr(UART_MANUAL_RFR, stop ? (1u << 31 | 1u << 1) : 0);
}

/* port_setup + geni_se_init + select FIFO mode + set_termios (8N1, RTS/CTS) + start_rx, as Linux does */
static bool
uart_init(void)
{
	uint32_t fw = rd(GENI_FW_REVISION_RO);

	step("the serial engine as a UART: protocol checked, FIFO mode, 8N1, RTS/CTS, 115200, reading started");
	show_se();
	if (((fw >> 8) & 0xff) != PROTO_UART) {
		return fail("the serial engine's firmware is not the UART one (no SE firmware loading here)");
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return fail("the serial engine's FIFO interface is disabled (GSI only): no polled UART possible");
	}
	tx_depth = (rd(SE_HW_PARAM_0) >> 16) & 0x3f;
	rx_depth = (rd(SE_HW_PARAM_1) >> 16) & 0x3f;
	if (tx_depth < 4 || rx_depth < 4) {
		return fail("the serial engine reports no FIFO");
	}
	stop_rx();
	if (rd(GENI_STATUS) & 1) {
		cancel_tx();
	}
	wr(UART_RX_STALE_CNT, 10 * 16);
	wr(GENI_TX_PACKING_CFG0, 0x4380e);      /* geni_se_config_packing(8 bits, 4 a word, LSB first) */
	wr(GENI_TX_PACKING_CFG1, 0xc3e0e);
	wr(GENI_RX_PACKING_CFG0, 0x4380e);
	wr(GENI_RX_PACKING_CFG1, 0xc3e0e);
	wr(GENI_BYTE_GRAN, 0);
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(GENI_S_IRQ_CLEAR, 0xffffffffu);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffffu);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffffu);
	wr(SE_IRQ_EN, 0xffffffffu);             /* status bits follow the enables; the line stays masked at the GIC */
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, 2);
	wr(GENI_RX_RFR_WATERMARK, rx_depth - 2);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | M_COMMON_IRQ);
	wr(GENI_S_IRQ_EN, rd(GENI_S_IRQ_EN) | S_COMMON_IRQ);
	wr(GENI_TX_WATERMARK, 0);
	wr(UART_LOOPBACK_CFG, 0);
	wr(UART_TX_TRANS_CFG, 0);               /* no parity, CTS obeyed */
	wr(UART_TX_PARITY_CFG, rd(UART_TX_PARITY_CFG) & ~1u);
	wr(UART_RX_TRANS_CFG, rd(UART_RX_TRANS_CFG) & ~(1u << 4));
	wr(UART_RX_PARITY_CFG, rd(UART_RX_PARITY_CFG) & ~1u);
	wr(UART_TX_WORD_LEN, 8);
	wr(UART_RX_WORD_LEN, 8);
	wr(UART_TX_STOP_BIT_LEN, 0);
	wr(UART_MANUAL_RFR, 0);
	if (!set_baud(115200)) {
		return false;
	}
	start_rx();
	se_ready = true;
	printf("bt: UART ready: FIFO %u/%u words, sampling %u, CTS reads %s\n", tx_depth, rx_depth, sampling,
	    (rd(GENI_IOS) & 2) ? "high (the chip holds us)" : "low (clear to send)");
	return true;
}

/* ---- H4: the parser, the queues, in-band sleep ---------------------------------------------------------------- */

#define H4_CMD                  1
#define H4_ACL                  2
#define H4_SCO                  3
#define H4_EVENT                4
#define H4_ISO                  5
#define IBS_WAKE_ACK            0xfc
#define IBS_WAKE_IND            0xfd
#define IBS_SLEEP_IND           0xfe
#define H4_MAX                  1200
#define EVENT_MAX               260

static struct {
	uint8_t buf[H4_MAX];
	uint32_t have, need, skip;
	bool header_done;
} rx;
static bool ibs_on, host_awake, ack_due, wake_acked;
static uint32_t ibs_wakes_in, ibs_sleeps_in;

/* the kernel's events, while it owns the link */
#define KQ_SLOTS 16
static uint8_t kq[KQ_SLOTS][EVENT_MAX];
static uint32_t kq_head, kq_tail;

/* user space's packets: [length lo, hi][packet] */
#define RING_SIZE 32768u
static uint8_t ring[RING_SIZE];
static uint32_t ring_head, ring_tail;   /* free running */
static uint64_t last_activity_ms;

static uint32_t
ring_used(void)
{
	return ring_tail - ring_head;
}

static void
ring_copy_in(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		ring[(ring_tail + i) % RING_SIZE] = p[i];
	}
	ring_tail += n;
}

static bool
ring_put(const uint8_t *p, uint32_t n)
{
	uint8_t len[2] = { (uint8_t)n, (uint8_t)(n >> 8) };

	if (RING_SIZE - ring_used() < n + 2) {
		return false;
	}
	ring_copy_in(len, 2);
	ring_copy_in(p, n);
	return true;
}

static uint32_t
ring_peek_len(void)
{
	return ring_used() < 2 ? 0 : (uint32_t)(ring[ring_head % RING_SIZE] | ring[(ring_head + 1) % RING_SIZE] << 8);
}

static uint32_t
ring_get(uint8_t *out, uint32_t room)
{
	uint32_t n = ring_peek_len();

	if (n == 0 || n > room) {
		return 0;
	}
	for (uint32_t i = 0; i < n; i++) {
		out[i] = ring[(ring_head + 2 + i) % RING_SIZE];
	}
	ring_head += n + 2;
	return n;
}

static void
log_event(const char *why, const uint8_t *p, uint32_t n)
{
	static uint32_t lines;

	if (lines++ < 32) {
		printf("bt: %s: event %#04x, %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", why, p[1], n, p[1], p[2],
		    n > 3 ? p[3] : 0, n > 4 ? p[4] : 0, n > 5 ? p[5] : 0, n > 6 ? p[6] : 0, n > 7 ? p[7] : 0, n > 8 ? p[8] : 0);
	}
}

static void
deliver(const uint8_t *p, uint32_t n)
{
	packets_in++;
	last_activity_ms = now_ms();
	if (p[0] == H4_EVENT && p[1] == 0x10) {
		link_broken = true;
		printf("bt: the chip reports a HARDWARE ERROR (code %#x)\n", n > 3 ? p[3] : 0);
	}
	if (kernel_owns) {
		if (p[0] == H4_EVENT && kq_tail - kq_head < KQ_SLOTS && n <= EVENT_MAX) {
			memcpy(kq[kq_tail++ % KQ_SLOTS], p, n);
			return;
		}
		dropped++;
		return;
	}
	if (!serving) {
		dropped++;
		return;
	}
	if (!ring_put(p, n)) {
		dropped++;
		errors++;
	}
	wakeup(&ring_head);
}

static void
ibs_byte(uint8_t b)
{
	ibs_wakes_in += b == IBS_WAKE_IND;
	ibs_sleeps_in += b == IBS_SLEEP_IND;
	ack_due = ack_due || b == IBS_WAKE_IND;         /* answered by the next pump, outside any transfer */
	wake_acked = wake_acked || b == IBS_WAKE_ACK;
}

static uint32_t
header_size(uint8_t type)
{
	static const uint8_t sizes[6] = { 0, 0, 4, 3, 2, 4 };

	return type < 6 ? sizes[type] : 0;
}

static uint32_t
payload_size(const uint8_t *p)
{
	switch (p[0]) {
	case H4_EVENT:
		return p[2];
	case H4_SCO:
		return p[3];
	case H4_ISO:
		return (uint32_t)(p[3] | p[4] << 8) & 0x3fff;
	default:
		return (uint32_t)(p[3] | p[4] << 8);
	}
}

static void
parse_byte(uint8_t b)
{
	if (rx.skip) {
		rx.skip--;
		return;
	}
	if (rx.have == 0) {
		if (b == IBS_WAKE_ACK || b == IBS_WAKE_IND || b == IBS_SLEEP_IND) {
			ibs_byte(b);
			return;
		}
		if (b == H4_CMD || header_size(b) == 0) {
			errors++;               /* out of step: dropped until a packet type comes */
			return;
		}
		rx.need = 1 + header_size(b);
		rx.header_done = false;
	}
	rx.buf[rx.have++] = b;
	if (rx.have < rx.need) {
		return;
	}
	if (!rx.header_done) {
		rx.header_done = true;
		rx.need += payload_size(rx.buf);
		if (rx.need > H4_MAX) {
			errors++;
			rx.skip = rx.need - rx.have;
			rx.have = 0;
			return;
		}
		if (rx.have < rx.need) {
			return;
		}
	}
	deliver(rx.buf, rx.have);
	rx.have = 0;
}

static void
note_rx_errors(void)
{
	uint32_t s = rd(GENI_S_IRQ_STATUS);

	if (s & (S_RX_OVERRUN | S_PARITY | S_FRAMING | S_BREAK)) {
		errors++;
		if (errors < 16) {
			printf("bt: UART receive error: status %#x (overrun 0x2000000, parity 0x200, framing 0x400, break 0x1800)\n", s);
		}
	}
	wr(GENI_S_IRQ_CLEAR, s);
	wr(GENI_M_IRQ_CLEAR, rd(GENI_M_IRQ_STATUS) & M_RX_IRQS);
}

/* whatever the RX FIFO holds, through the parser (bt_lock held) */
static void
drain_rx(void)
{
	for (int rounds = 0; rounds < 64; rounds++) {
		uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, last = (status >> 28) & 7, bytes;

		if (words == 0) {
			break;
		}
		bytes = 4 * (words - 1) + ((status & (1u << 31)) && last ? last : 4);
		for (uint32_t w = 0; w < words; w++) {
			uint32_t word = rd(GENI_RX_FIFO);

			for (uint32_t i = 0; i < 4 && bytes; i++, bytes--) {
				parse_byte((uint8_t)(word >> (8 * i)));
			}
		}
	}
	note_rx_errors();
}

/* one transfer of `n` bytes out of the TX FIFO (bt_lock held); the RX side is drained while waiting */
static bool
uart_tx(const uint8_t *p, uint32_t n)
{
	uint64_t deadline = now_ms() + 500 + (uint64_t)n * 10 * 1000 * 3 / (baud ? baud : 115200);
	uint32_t sent = 0;

	while (rd(GENI_STATUS) & 1) {
		if (now_ms() > deadline) {
			cancel_tx();
			break;
		}
		drain_rx();
	}
	wr(GENI_M_IRQ_CLEAR, M_CMD_DONE);
	wr(UART_TX_TRANS_LEN, n);
	wr(GENI_M_CMD0, UART_START_TX);
	while (now_ms() <= deadline) {
		while (sent < n && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) < tx_depth) {
			uint32_t word = 0;

			for (uint32_t i = 0; i < 4 && sent < n; i++, sent++) {
				word |= (uint32_t)p[sent] << (8 * i);
			}
			wr(GENI_TX_FIFO, word);
		}
		if (sent == n && (rd(GENI_M_IRQ_STATUS) & M_CMD_DONE)) {
			wr(GENI_M_IRQ_CLEAR, M_CMD_DONE);
			return true;
		}
		drain_rx();
	}
	errors++;
	printf("bt: UART transfer of %u bytes timed out (sent %u to the FIFO, CTS %s, irq %#x)\n", n, sent,
	    (rd(GENI_IOS) & 2) ? "held by the chip" : "clear", rd(GENI_M_IRQ_STATUS));
	cancel_tx();
	return false;
}

static void
pump_locked(void)
{
	static const uint8_t ack = IBS_WAKE_ACK;

	drain_rx();
	if (ack_due) {
		ack_due = false;
		(void)uart_tx(&ack, 1);
	}
}

static void
pump(void)
{
	lck_mtx_lock(&bt_lock);
	pump_locked();
	lck_mtx_unlock(&bt_lock);
}

/* hci_qca after setup: the host's side starts asleep, so its first packet is preceded by WAKE_IND until WAKE_ACK */
static void
host_wake(void)
{
	static const uint8_t ind = IBS_WAKE_IND;

	for (int tries = 0; tries < 5 && !wake_acked; tries++) {
		(void)uart_tx(&ind, 1);
		for (int i = 0; i < 100 && !wake_acked; i++) {
			lck_mtx_unlock(&bt_lock);
			sleep_ms(1);
			lck_mtx_lock(&bt_lock);
			drain_rx();
		}
	}
	printf("bt: in-band sleep: host awake (%s); it never sends SLEEP_IND\n", wake_acked ? "the chip acknowledged" :
	    "NO WAKE_ACK in 500 ms, going on");
	host_awake = true;
}

/* one whole H4 packet to the chip */
static bool
link_send(const uint8_t *p, uint32_t n)
{
	bool ok;

	lck_mtx_lock(&bt_lock);
	if (ibs_on && !host_awake) {
		host_wake();
	}
	ok = uart_tx(p, n);
	packets_out += ok;
	last_activity_ms = now_ms();
	lck_mtx_unlock(&bt_lock);
	wakeup(&last_activity_ms);
	return ok;
}

/* ---- HCI commands from the kernel ----------------------------------------------------------------------------- */

struct reply {
	uint8_t data[EVENT_MAX];        /* Command Complete's return parameters, or Command Status's status */
	uint32_t len;
};

typedef void (*event_sink_t)(const uint8_t *event, uint32_t n);
static event_sink_t event_sink;

static bool
kq_get(uint8_t *out)
{
	bool got;

	lck_mtx_lock(&bt_lock);
	got = kq_head != kq_tail;
	if (got) {
		memcpy(out, kq[kq_head++ % KQ_SLOTS], EVENT_MAX);
	}
	lck_mtx_unlock(&bt_lock);
	return got;
}

static void
other_event(const uint8_t *e, uint32_t n)
{
	if (event_sink != NULL) {
		event_sink(e, n);
		return;
	}
	log_event(e[1] == 0xff ? "vendor event (ignored)" : "event not waited for", e, n);
}

/* the events queued for the kernel, none of them awaited */
static void
consume_events(void)
{
	static uint8_t e[EVENT_MAX];

	while (kq_get(e)) {
		other_event(e, 3u + e[2]);
	}
}

/* events until `done` says one is the answer, or the time is up */
static bool
wait_event(bool (*done)(const uint8_t *e, uint32_t n, void *context), void *context, uint32_t timeout_ms)
{
	static uint8_t e[EVENT_MAX];
	uint64_t deadline = now_ms() + timeout_ms;

	while (now_ms() <= deadline && !link_broken) {
		pump();
		while (kq_get(e)) {
			uint32_t n = 3u + e[2];

			if (done(e, n, context)) {
				return true;
			}
			other_event(e, n);
		}
		sleep_ms(1);
	}
	return false;
}

struct command_wait {
	uint16_t opcode;
	bool status_only;
	struct reply *reply;
	bool failed;
};

static bool
command_answer(const uint8_t *e, uint32_t n, void *context)
{
	struct command_wait *w = context;
	bool complete = e[1] == 0x0e && n >= 6 && (uint16_t)(e[4] | e[5] << 8) == w->opcode;
	bool status = e[1] == 0x0f && n >= 7 && (uint16_t)(e[5] | e[6] << 8) == w->opcode;

	if (complete) {
		w->reply->len = n - 6;
		memcpy(w->reply->data, e + 6, n - 6);
		return true;
	}
	if (status) {
		w->reply->len = 1;
		w->reply->data[0] = e[3];
		w->failed = e[3] != 0 || !w->status_only;
		return true;
	}
	return false;
}

/* a command, then its Command Complete (or, with status_only, Command Status); false on timeout or a failure status */
static bool
hci_command(uint16_t opcode, const uint8_t *params, uint8_t plen, struct reply *reply, bool status_only, uint32_t timeout_ms)
{
	static uint8_t packet[4 + 255];
	struct command_wait w = { opcode, status_only, reply, false };

	packet[0] = H4_CMD;
	packet[1] = (uint8_t)opcode;
	packet[2] = (uint8_t)(opcode >> 8);
	packet[3] = plen;
	if (plen != 0) {
		memcpy(packet + 4, params, plen);
	}
	reply->len = 0;
	if (!link_send(packet, 4u + plen)) {
		printf("bt: command %#06x could not be sent\n", opcode);
		return false;
	}
	if (!wait_event(command_answer, &w, timeout_ms)) {
		printf("bt: command %#06x: no answer in %u ms%s\n", opcode, timeout_ms, link_broken ? " (hardware error)" : "");
		return false;
	}
	if (w.failed) {
		printf("bt: command %#06x: status %#x\n", opcode, reply->data[0]);
		return false;
	}
	return true;
}

/* Command Complete with status 0 as its first return parameter */
static bool
hci_simple(uint16_t opcode, const uint8_t *params, uint8_t plen, const char *what)
{
	struct reply r;

	if (!hci_command(opcode, params, plen, &r, false, 2000)) {
		return fail(what);
	}
	if (r.len < 1 || r.data[0] != 0) {
		printf("bt: command %#06x answered status %#x\n", opcode, r.len ? r.data[0] : 0xff);
		return fail(what);
	}
	return true;
}

/* ---- the chip: power, ROM, QCA setup (btqca.c, hci_qca.c for QCA_WCN6750) -------------------------------------- */

#define EDL_PATCH_CMD           0xfc00
#define EDL_PATCH_VER_REQ       0x19
#define EDL_PATCH_TLV_REQ       0x1e
#define EDL_GET_BUILD_INFO      0x20
#define EDL_PATCH_CONFIG        0x28
#define QCA_DISABLE_LOGGING     0xfc17
#define QCA_SET_BAUDRATE        0xfc48
#define QCA_BAUDRATE_115200     0
#define QCA_BAUDRATE_3000000    14
#define TLV_SEGMENT             243

static uint8_t nvm_baud_code = QCA_BAUDRATE_115200;

static bool
chip_on(void)
{
	step("BT_EN low 150 ms (hci_qca 50 ms, Xiaomi's btpower 100 ms more when Wi-Fi is off), high, 100 ms, SW_CTRL read");
	lck_mtx_lock(&bt_lock);
	stop_rx();
	lck_mtx_unlock(&bt_lock);
	en_driven = true;
	bt_en(false);
	sleep_ms(150);
	bt_en(true);
	sleep_ms(100);
	printf("bt: BT_EN reads %u, SW_CTRL reads %u (1 = the chip is up), CTS %s\n", rd(GPIO_IO(PIN_BT_EN)) & 1,
	    rd(GPIO_IO(PIN_SW_CTRL)) & 1, (rd(GENI_IOS) & 2) ? "held" : "clear");
	lck_mtx_lock(&bt_lock);
	rx.have = rx.skip = 0;
	start_rx();
	drain_rx();
	lck_mtx_unlock(&bt_lock);
	state = "powered";
	return true;
}

static bool
read_version(void)
{
	static const uint8_t cmd = EDL_PATCH_VER_REQ;
	struct reply r;
	uint32_t soc_ver;

	step("QCA get version (EDL 0xfc00, 0x19)");
	if (!hci_command(EDL_PATCH_CMD, &cmd, 1, &r, false, 3000)) {
		return fail("the chip does not answer the version request");
	}
	/* btqca.c for WCN3991 and later: status, 0x19, one byte, then product, patch, ROM, SoC (15 bytes) */
	if (r.len != 15 || r.data[0] != 0 || r.data[1] != EDL_PATCH_VER_REQ) {
		printf("bt: version answer: %u bytes, %02x %02x\n", r.len, r.data[0], r.data[1]);
		return fail("the version answer is not the one btqca.c expects");
	}
	chip.product = (uint32_t)(r.data[3] | r.data[4] << 8 | r.data[5] << 16 | (uint32_t)r.data[6] << 24);
	chip.patch = (uint16_t)(r.data[7] | r.data[8] << 8);
	chip.rom = (uint16_t)(r.data[9] | r.data[10] << 8);
	chip.soc = (uint32_t)(r.data[11] | r.data[12] << 8 | r.data[13] << 16 | (uint32_t)r.data[14] << 24);
	soc_ver = chip.soc << 16 | chip.rom;
	chip.rom_code = (uint8_t)(((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf));
	chip.known = true;
	printf("bt: QCA product %#010x, SoC %#010x, ROM %#06x, patch %#06x: controller version %#010x, firmware set %02x\n",
	    chip.product, chip.soc, chip.rom, chip.patch, soc_ver, chip.rom_code);
	if (chip.soc == 0 || chip.rom == 0) {
		return fail("SoC or ROM version 0");
	}
	return chip.rom_code == 0x11 ? true : fail("not ROM 0x11: the firmware here (msbtfw11/msnv11) is not for it");
}

static bool
read_local_version(void)
{
	struct reply r;

	if (!hci_command(0x1001, NULL, 0, &r, false, 2000) || r.len < 9 || r.data[0] != 0) {
		return fail("Read Local Version Information failed");
	}
	printf("bt: local version: HCI %u rev %#06x, LMP %u subversion %#06x, manufacturer %u%s\n", r.data[1],
	    r.data[2] | r.data[3] << 8, r.data[4], r.data[7] | r.data[8] << 8, r.data[5] | r.data[6] << 8,
	    (r.data[5] | r.data[6] << 8) == 29 ? " (Qualcomm)" : "");
	return true;
}

static bool
rom_reset_and_version(void)
{
	step("HCI reset and Read Local Version, in ROM (bt=2 only: Linux does not reset before the rampatch)");
	return hci_simple(0x0c03, NULL, 0, "HCI reset failed") && read_local_version();
}

/* hci_qca qca_set_speed(QCA_OPER_SPEED): RTS held, the command at the old rate, then ours, then flow again */
static bool
go_fast(void)
{
	static const uint8_t cmd[5] = { H4_CMD, (uint8_t)QCA_SET_BAUDRATE, QCA_SET_BAUDRATE >> 8, 1, QCA_BAUDRATE_3000000 };
	static const uint8_t version_request = EDL_PATCH_VER_REQ;
	struct reply r;
	bool sent;

	if (!fast_wanted || se_hz != (uint64_t)FAST_BAUD * sampling) {
		printf("bt: staying at 115200 (%s)\n", fast_wanted ? "3 Mbit/s would not be exact" : "bt_fast=0");
		return true;
	}
	step("baud change to 3000000 (vendor 0xfc48, code 14): RTS held, sent at 115200, 10 ms, ours changed, flow again");
	lck_mtx_lock(&bt_lock);
	flow_stopped(true);
	sent = uart_tx(cmd, sizeof(cmd));
	lck_mtx_unlock(&bt_lock);
	packets_out += sent;
	sleep_ms(10);
	lck_mtx_lock(&bt_lock);
	stop_rx();
	sent = sent && set_baud(FAST_BAUD);
	rx.have = rx.skip = 0;
	start_rx();
	flow_stopped(false);
	lck_mtx_unlock(&bt_lock);
	if (!sent) {
		return fail("the baud change could not be made");
	}
	nvm_baud_code = QCA_BAUDRATE_3000000;
	sleep_ms(20);
	if (!hci_command(EDL_PATCH_CMD, &version_request, 1, &r, false, 2000)) {
		return fail("the chip does not answer at 3 Mbit/s");
	}
	printf("bt: the chip answers at 3000000 baud\n");
	return true;
}

static void
patch_config(void)
{
	static const uint8_t cmd[5] = { EDL_PATCH_CONFIG, 0x01, 0, 0, 0 };
	struct reply r;
	bool ok;

	step("QCA patch config (EDL 0x28; btqca.c goes on whatever it answers)");
	ok = hci_command(EDL_PATCH_CMD, cmd, sizeof(cmd), &r, false, 2000);
	printf("bt: patch config: %s, %u bytes %02x %02x\n", ok ? "answered" : "NO ANSWER", r.len, r.data[0], r.data[1]);
}

static bool
segment_acked(const struct reply *r)
{
	return r->len == 2 && r->data[0] == 0 && r->data[1] == EDL_PATCH_TLV_REQ;
}

/* qca_tlv_send_segment: EDL 0x1e, length, data; only the waited ones get a Command Complete */
static bool
send_segment(const uint8_t *data, uint32_t n, bool wait)
{
	static uint8_t packet[4 + 2 + TLV_SEGMENT];
	struct reply r;

	if (!wait) {
		packet[0] = H4_CMD;
		packet[1] = (uint8_t)EDL_PATCH_CMD;
		packet[2] = EDL_PATCH_CMD >> 8;
		packet[3] = (uint8_t)(n + 2);
		packet[4] = EDL_PATCH_TLV_REQ;
		packet[5] = (uint8_t)n;
		memcpy(packet + 6, data, n);
		if (!link_send(packet, 6 + n)) {
			return false;
		}
		pump();
		consume_events();
		return !link_broken;
	}
	packet[0] = EDL_PATCH_TLV_REQ;
	packet[1] = (uint8_t)n;
	memcpy(packet + 2, data, n);
	if (!hci_command(EDL_PATCH_CMD, packet, (uint8_t)(n + 2), &r, false, 3000)) {
		return false;
	}
	if (!segment_acked(&r)) {
		printf("bt: segment answer: %u bytes %02x %02x\n", r.len, r.data[0], r.data[1]);
		return false;
	}
	return true;
}

/* qca_download_firmware's loop: unacknowledged segments when `skip`, the last one always acknowledged */
static bool
download(const char *name, const uint8_t *data, uint64_t size, bool skip)
{
	uint64_t start = now_ms();
	uint32_t count = (uint32_t)((size + TLV_SEGMENT - 1) / TLV_SEGMENT);

	printf("bt: downloading %s: %llu bytes, %u segments, %s\n", name, size, count, skip ? "only the last acknowledged" :
	    "each acknowledged");
	for (uint32_t i = 0; i < count; i++) {
		uint32_t n = (uint32_t)MIN(TLV_SEGMENT, size - (uint64_t)i * TLV_SEGMENT);
		bool last = i + 1 == count || n < TLV_SEGMENT;

		if (!send_segment(data + (uint64_t)i * TLV_SEGMENT, n, !skip || last)) {
			printf("bt: %s: segment %u of %u failed\n", name, i + 1, count);
			return false;
		}
		if ((i + 1) % 200 == 0) {
			printf("bt: %s: %u of %u segments\n", name, i + 1, count);
		}
	}
	printf("bt: %s downloaded in %llu ms\n", name, now_ms() - start);
	return true;
}

static bool
rampatch(void)
{
	uint64_t size = 0;
	const uint8_t *mbn = dlisa_periph_file("msbtfw11.mbn", &size), *tlv;

	step("rampatch msbtfw11.mbn (ELF, btqca.c's first choice), else msbtfw11.tlv");
	if (mbn != NULL && size >= 7 && mbn[0] == 0x7f && mbn[1] == 'E' && mbn[2] == 'L' && mbn[3] == 'F') {
		printf("bt: msbtfw11.mbn: class %u, encoding %u, version %u\n", mbn[4], mbn[5], mbn[6]);
		strlcpy(fw_text, "msbtfw11.mbn", sizeof(fw_text));
		return download("msbtfw11.mbn", mbn, size, true) ? true : fail("the rampatch download failed");
	}
	printf("bt: msbtfw11.mbn %s: trying msbtfw11.tlv\n", mbn == NULL ? "not found" : "is not an ELF file");
	tlv = dlisa_periph_file("msbtfw11.tlv", &size);
	if (tlv == NULL || size < 28 || tlv[0] != 1) {
		return fail("no usable rampatch (msbtfw11.mbn or .tlv; periph-fw.py builds them in)");
	}
	/* struct tlv_type_patch after the 4-byte header: download mode at byte 14 of the file; 0 = every segment acked */
	printf("bt: msbtfw11.tlv: download mode %u, product %#x, ROM build %#x, patch %#x\n", tlv[14], tlv[16] | tlv[17] << 8,
	    tlv[18] | tlv[19] << 8, tlv[20] | tlv[21] << 8);
	strlcpy(fw_text, "msbtfw11.tlv", sizeof(fw_text));
	return download("msbtfw11.tlv", tlv, size, tlv[14] != 0) ? true : fail("the rampatch download failed");
}

static uint8_t nvm_copy[8192];

/* qca_tlv_check_data for TLV_TYPE_NVM: HCI tag (17) byte 0 |= 0x80 (in-band sleep), byte 1 = our baud; deep sleep (27) |= 1 */
static bool
edit_nvm(uint32_t size)
{
	uint32_t at = 0, type_len = (uint32_t)(nvm_copy[0] | nvm_copy[1] << 8 | nvm_copy[2] << 16 | (uint32_t)nvm_copy[3] << 24);
	uint32_t length, idx = 0;

	if ((type_len & 0xff) == 4) {
		at = 4;
		type_len = (uint32_t)(nvm_copy[4] | nvm_copy[5] << 8 | nvm_copy[6] << 16 | (uint32_t)nvm_copy[7] << 24);
	}
	length = type_len >> 8;
	if ((type_len & 0xff) != 2 || at + 4 + length > size) {
		printf("bt: msnv11.bin: TLV type %u, length %u of %u: not edited\n", type_len & 0xff, length, size);
		return (type_len & 0xff) != 2;
	}
	while (idx + 12 <= length) {
		uint8_t *tag = nvm_copy + at + 4 + idx;
		uint32_t id = (uint32_t)(tag[0] | tag[1] << 8), len = (uint32_t)(tag[2] | tag[3] << 8);

		if (idx + 12 + len > length) {
			return false;
		}
		if (id == 2 && len == 6) {
			printf("bt: NVM BD_ADDR tag: %02X:%02X:%02X:%02X:%02X:%02X\n", tag[17], tag[16], tag[15], tag[14], tag[13], tag[12]);
		}
		if (id == 17 && len >= 3) {
			printf("bt: NVM HCI tag: %02x %02x -> %02x %02x\n", tag[12], tag[13], ibs_wanted ? tag[12] | 0x80 : tag[12],
			    nvm_baud_code);
			tag[12] = ibs_wanted ? tag[12] | 0x80 : tag[12];
			tag[13] = nvm_baud_code;
		}
		if (id == 27 && len >= 1 && ibs_wanted) {
			tag[12] |= 1;
		}
		idx += 12 + len;
	}
	return true;
}

static bool
nvm(void)
{
	uint64_t size = 0;
	const uint8_t *file = dlisa_periph_file("msnv11.bin", &size);

	step("NVM msnv11.bin (each segment acknowledged), with btqca.c's tag edits");
	if (file == NULL || size < 8 || size > sizeof(nvm_copy)) {
		return fail("no usable msnv11.bin");
	}
	memcpy(nvm_copy, file, size);
	if (!edit_nvm((uint32_t)size)) {
		return fail("msnv11.bin's tags do not parse");
	}
	strlcat(fw_text, "+msnv11.bin", sizeof(fw_text));
	return download("msnv11.bin", nvm_copy, size, false) ? true : fail("the NVM download failed");
}

static bool
build_info(void)
{
	static const uint8_t cmd = EDL_GET_BUILD_INFO;
	struct reply r;
	uint32_t n;

	if (!hci_command(EDL_PATCH_CMD, &cmd, 1, &r, false, 2000) || r.len < 3 || r.data[0] != 0 || r.data[1] != EDL_GET_BUILD_INFO) {
		return fail("the firmware build info (EDL 0x20) did not come");
	}
	n = MIN(r.data[2], MIN(r.len - 3, sizeof(build_label) - 1));
	memcpy(build_label, r.data + 3, n);
	build_label[n] = 0;
	printf("bt: firmware build: %s\n", build_label);
	return true;
}

static bool
read_bdaddr(void)
{
	struct reply r;
	const uint8_t *a = r.data + 1;

	if (!hci_command(0x1009, NULL, 0, &r, false, 2000) || r.len != 7 || r.data[0] != 0) {
		return fail("Read BD_ADDR failed");
	}
	snprintf(bdaddr_text, sizeof(bdaddr_text), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
	printf("bt: BD_ADDR %s%s\n", bdaddr_text, (a[5] | a[4] | a[3] | a[2]) == 0 ? " (the NVM's placeholder: the board "
	    "gives no address of its own, qcom,local-bd-address-broken; Linux leaves it unconfigured until one is set)" : "");
	return true;
}

static bool
qca_setup(void)
{
	static const uint8_t no_logging[2] = { 0x14, 0x00 };
	bool ok = go_fast();

	if (!ok) {
		return false;
	}
	patch_config();
	ok = rampatch();
	sleep_ms(10);
	ok = ok && nvm();
	if (ok) {
		step("SoC logging off (0xfc17), HCI reset with the firmware, build info, BD_ADDR, local version");
		ok = hci_simple(QCA_DISABLE_LOGGING, no_logging, 2, "SoC logging off failed");
		ok = ok && hci_simple(0x0c03, NULL, 0, "HCI reset failed");
		ok = ok && build_info() && read_bdaddr() && read_local_version();
	}
	ibs_on = ok && ibs_wanted;
	state = ok ? "ready" : state;
	printf("bt: QCA setup %s (%s, in-band sleep %s)\n", ok ? "complete" : "FAILED", fw_text, ibs_on ? "on" : "off");
	return ok;
}

/* ---- discovery: LE scan, classic inquiry ------------------------------------------------------------------------ */

#define SEEN_MAX 48
static struct {
	uint8_t addr[6];
	int8_t rssi;
	bool le;
	char name[32];
} seen[SEEN_MAX];
static uint32_t seen_count;

static void
find_name(const uint8_t *ad, uint32_t n, char *out, uint32_t room)
{
	for (uint32_t i = 0; i + 1 < n && ad[i] != 0; i += 1u + ad[i]) {
		uint32_t len = ad[i];

		if (i + 1 + len > n || (ad[i + 1] != 0x08 && ad[i + 1] != 0x09)) {
			continue;
		}
		len = MIN(len - 1, room - 1);
		memcpy(out, ad + i + 2, len);
		out[len] = 0;
		return;
	}
}

static void
saw(const uint8_t *addr, int8_t rssi, bool le, const uint8_t *ad, uint32_t ad_len, const char *kind)
{
	char name[32] = "";
	uint32_t i;

	find_name(ad, ad_len, name, sizeof(name));
	for (i = 0; i < seen_count && memcmp(seen[i].addr, addr, 6) != 0; i++) {
	}
	if (i == seen_count && seen_count < SEEN_MAX) {
		memcpy(seen[i].addr, addr, 6);
		seen[i].le = le;
		seen[i].name[0] = 0;
		seen_count++;
		printf("bt: found %s %02X:%02X:%02X:%02X:%02X:%02X (%s) rssi %d dBm%s%s%s\n", le ? "LE" : "BR/EDR", addr[5], addr[4],
		    addr[3], addr[2], addr[1], addr[0], kind, rssi, name[0] ? " name \"" : "", name, name[0] ? "\"" : "");
	}
	if (i < seen_count) {
		seen[i].rssi = rssi;
		if (name[0] && seen[i].name[0] == 0) {
			strlcpy(seen[i].name, name, sizeof(seen[i].name));
			printf("bt: %02X:%02X:%02X:%02X:%02X:%02X is \"%s\"\n", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], name);
		}
	}
}

/* LE Advertising Report (LE meta 0x02): each report is type, address type, address, length, data, RSSI */
static void
le_reports(const uint8_t *e, uint32_t n)
{
	static const char *const types[] = { "connectable", "directed", "scannable", "non-connectable", "scan response" };
	uint32_t at = 5;

	for (uint32_t r = 0; r < e[4] && at + 9 <= n; r++) {
		uint32_t len = e[at + 8];

		if (at + 9 + len + 1 > n) {
			break;
		}
		saw(e + at + 2, (int8_t)e[at + 9 + len], true, e + at + 9, len, e[at] < 5 ? types[e[at]] : "?");
		at += 10 + len;
	}
}

static void
discovery_event(const uint8_t *e, uint32_t n)
{
	if (e[1] == 0x3e && n > 5 && e[3] == 0x02) {
		le_reports(e, n);
	} else if (e[1] == 0x2f && n >= 3 + 15) {      /* Extended Inquiry Result */
		saw(e + 4, (int8_t)e[17], false, e + 18, n - 18, "inquiry, extended");
	} else if (e[1] == 0x22 && n >= 4 + 14) {      /* Inquiry Result with RSSI */
		saw(e + 4, (int8_t)e[17], false, NULL, 0, "inquiry");
	} else if (e[1] == 0x02 && n >= 4 + 14) {      /* Inquiry Result */
		saw(e + 4, 0, false, NULL, 0, "inquiry, no RSSI");
	} else {
		log_event("event during discovery", e, n);
	}
}

static bool
never(__unused const uint8_t *e, __unused uint32_t n, __unused void *context)
{
	return false;
}

static bool
inquiry_complete(const uint8_t *e, __unused uint32_t n, __unused void *context)
{
	return e[1] == 0x01;
}

static bool
scan_le(uint32_t secs)
{
	static const uint8_t mask[8] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };      /* LE meta, inquiry RSSI/EIR */
	static const uint8_t params[7] = { 0x01, 0x60, 0x00, 0x30, 0x00, 0x01, 0x00 };          /* active, 60/30 ms, random */
	static const uint8_t on[2] = { 1, 1 }, off[2] = { 0, 0 };
	uint64_t t = mach_absolute_time();
	uint8_t random[6] = { (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24), (uint8_t)(t >> 32),
		              (uint8_t)((t >> 40) | 0xc0) };   /* a static random address: the board has no address */
	uint32_t before = seen_count;
	bool ok;

	step("LE scan: event mask, a random static address, active scanning, duplicates filtered");
	printf("bt: LE scan for %u s from %02X:%02X:%02X:%02X:%02X:%02X (random static)\n", secs, random[5], random[4],
	    random[3], random[2], random[1], random[0]);
	ok = hci_simple(0x0c01, mask, 8, "Set Event Mask failed") && hci_simple(0x2005, random, 6, "LE Set Random Address failed");
	ok = ok && hci_simple(0x200b, params, 7, "LE Set Scan Parameters failed") && hci_simple(0x200c, on, 2, "LE scan enable failed");
	if (!ok) {
		return false;
	}
	event_sink = discovery_event;
	(void)wait_event(never, NULL, secs * 1000);
	event_sink = NULL;
	ok = hci_simple(0x200c, off, 2, "LE scan disable failed");
	scan_found = seen_count - before;
	scans_done++;
	printf("bt: LE scan done: %u new devices, %u known in all\n", scan_found, seen_count);
	return ok;
}

static bool
inquiry(void)
{
	static const uint8_t mode = 2, giac[5] = { 0x33, 0x8b, 0x9e, 8, 0 };   /* 8 * 1.28 s, unlimited responses */
	struct reply r;
	bool done;

	step("classic inquiry: extended results, GIAC, 10.24 s");
	if (!hci_simple(0x0c45, &mode, 1, "Write Inquiry Mode failed")) {
		return false;
	}
	if (!hci_command(0x0401, giac, 5, &r, true, 2000)) {
		return fail("Inquiry was not accepted");
	}
	event_sink = discovery_event;
	done = wait_event(inquiry_complete, NULL, 16000);
	event_sink = NULL;
	printf("bt: inquiry %s: %u devices known in all\n", done ? "complete" : "DID NOT COMPLETE in 16 s", seen_count);
	return done ? true : fail("no Inquiry Complete");
}

/* ---- giving up ---------------------------------------------------------------------------------------------- */

static void
give_up(void)
{
	printf("bt: giving up (%s): BT_EN low, the UART and clocks put back\n", failed_at);
	state = "failed";
	serving = false;
	en_driven ? bt_en(false) : (void)0;
	if (se_ready) {
		lck_mtx_lock(&bt_lock);
		stop_rx();
		if (rd(GENI_STATUS) & 1) {
			cancel_tx();
		}
		se_ready = false;
		lck_mtx_unlock(&bt_lock);
	}
	give_back_pins();
	give_back_clocks();
	for (unsigned i = 0; i < SUPPLIES; i++) {
		if (rail_voted[i]) {
			printf("bt: supply %s was voted on by this driver and is left on\n", supplies[i].rail);
		}
	}
}

/* ---- user space: /dev/hakios-bt, the kern_ctl bridge, sysctls ------------------------------------------------- */

#define USER_WRITE_MAX 4096

/* the next whole packet at p (n bytes there): its length, 0 if incomplete, -1 if not a packet to send */
static int
user_packet_len(const uint8_t *p, uint32_t n)
{
	uint32_t header = p[0] == H4_CMD ? 3 : (p[0] == H4_ACL || p[0] == H4_SCO || p[0] == H4_ISO) ? header_size(p[0]) : 0;
	uint32_t payload;

	if (header == 0) {
		return -1;
	}
	if (n < 1 + header) {
		return 0;
	}
	payload = p[0] == H4_CMD || p[0] == H4_SCO ? p[3] : payload_size(p);
	if (1 + header + payload > H4_MAX) {
		return -1;
	}
	return n < 1 + header + payload ? 0 : (int)(1 + header + payload);
}

static int
link_available(void)
{
	if (!serving) {
		return ENODEV;
	}
	return kernel_owns ? EBUSY : 0;
}

static int
bt_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	static uint8_t buf[USER_WRITE_MAX];
	user_ssize_t asked = uio_resid(uio);
	uint32_t at = 0;
	int error = link_available();

	if (error != 0) {
		return error;
	}
	if (asked <= 0 || asked > USER_WRITE_MAX) {
		return asked <= 0 ? 0 : EMSGSIZE;
	}
	lck_mtx_lock(&bridge_lock);
	error = uiomove((const char *)buf, (int)asked, uio);
	for (uint32_t n = (uint32_t)asked; error == 0 && at < n;) {          /* every packet whole before any is sent */
		int len = user_packet_len(buf + at, n - at);

		error = len <= 0 ? EINVAL : 0;
		at += len > 0 ? (uint32_t)len : 0;
	}
	for (at = 0; error == 0 && at < (uint32_t)asked;) {
		uint32_t len = (uint32_t)user_packet_len(buf + at, (uint32_t)asked - at);

		error = link_send(buf + at, len) ? 0 : EIO;
		at += len;
	}
	lck_mtx_unlock(&bridge_lock);
	return error;
}

static bool bridge_connected;

static int
bt_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	static uint8_t packet[H4_MAX];
	int error = 0, moved = 0;

	if (!serving) {
		return ENODEV;
	}
	lck_mtx_lock(&bt_lock);
	while (ring_used() == 0 || bridge_connected) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&bt_lock);
			return EWOULDBLOCK;
		}
		error = msleep(&ring_head, &bt_lock, PZERO | PCATCH, "hakios-bt", NULL);
		if (error != 0) {
			lck_mtx_unlock(&bt_lock);
			return error;
		}
	}
	while (error == 0 && ring_used() != 0 && (user_ssize_t)ring_peek_len() <= uio_resid(uio)) {
		uint32_t n = ring_get(packet, sizeof(packet));

		lck_mtx_unlock(&bt_lock);
		error = uiomove((const char *)packet, (int)n, uio);
		lck_mtx_lock(&bt_lock);
		moved += error == 0;
	}
	error = moved == 0 && error == 0 ? EINVAL : error;      /* the next packet is longer than the buffer */
	lck_mtx_unlock(&bt_lock);
	return moved ? 0 : error;
}

static int
bt_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw bt_cdevsw = {
	.d_open = bt_open, .d_close = bt_open, .d_read = bt_read, .d_write = bt_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The kern_ctl "com.apple.uart.bluetooth": what bluetoothd's H4 POSIX transport opens (PF_SYSTEM datagram socket,
 * TIOCGETA/TIOCSETA as socket options, then raw H4 both ways). One client at a time. Vendor commands that are not
 * Qualcomm's own (Broadcom's: bd address, sleep mode, baud, power tables...) are answered here with a Command Complete,
 * status 0; the baud change too (the kernel owns the link's rate).
 */
#define TIOCGETA_64_OPT         0x40487413
#define TIOCSETA_64_OPT         0x80487414
#define BRIDGE_LOG_FULL         64
#define BRIDGE_OPCODES          32

static kern_ctl_ref bridge_ref;
static uint32_t bridge_unit, bridge_have, bridge_cmds, bridge_local, bridge_out, bridge_drops, bridge_errors;
static uint8_t bridge_buf[USER_WRITE_MAX];
static struct {
	uint16_t opcode;
	uint32_t count;
} bridge_seen[BRIDGE_OPCODES];

static const uint16_t qualcomm_own[] = { 0xfc00, 0xfc0b, 0xfc0c, 0xfc14, 0xfc17, 0xfd70 };

static void
bridge_count(const uint8_t *p, uint32_t n)
{
	uint16_t opcode = (uint16_t)(p[1] | p[2] << 8);
	uint32_t i;

	if (bridge_cmds++ < BRIDGE_LOG_FULL) {
		printf("bt: bridge: command %#06x (OGF %#x OCF %#05x), %u parameter bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    opcode, opcode >> 10, opcode & 0x3ff, p[3], n > 4 ? p[4] : 0, n > 5 ? p[5] : 0, n > 6 ? p[6] : 0,
		    n > 7 ? p[7] : 0, n > 8 ? p[8] : 0, n > 9 ? p[9] : 0, n > 10 ? p[10] : 0, n > 11 ? p[11] : 0);
	}
	for (i = 0; i < BRIDGE_OPCODES && bridge_seen[i].count != 0 && bridge_seen[i].opcode != opcode; i++) {
	}
	if (i == BRIDGE_OPCODES) {
		return;
	}
	bridge_seen[i].opcode = opcode;
	bridge_seen[i].count++;
	if (bridge_cmds > BRIDGE_LOG_FULL && (bridge_seen[i].count & (bridge_seen[i].count - 1)) == 0) {
		printf("bt: bridge: command %#06x seen %u times\n", opcode, bridge_seen[i].count);
	}
}

static bool
answered_here(const uint8_t *p)
{
	uint16_t opcode = (uint16_t)(p[1] | p[2] << 8);
	uint8_t complete[7] = { H4_EVENT, 0x0e, 4, 1, p[1], p[2], 0 };

	if (p[0] != H4_CMD || (opcode >> 10) != 0x3f) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(qualcomm_own) / sizeof(qualcomm_own[0]); i++) {
		if (opcode == qualcomm_own[i]) {
			return false;
		}
	}
	bridge_local++;
	printf("bt: bridge: vendor command %#06x answered here (Command Complete, status 0)\n", opcode);
	lck_mtx_lock(&bt_lock);
	(void)ring_put(complete, sizeof(complete));
	lck_mtx_unlock(&bt_lock);
	wakeup(&last_activity_ms);
	return true;
}

/* whole packets out of the client's stream; the rest waits for its next send */
static void
bridge_packets(void)
{
	uint32_t at = 0;

	while (at < bridge_have) {
		int len = user_packet_len(bridge_buf + at, bridge_have - at);

		if (len < 0) {
			bridge_errors++;
			printf("bt: bridge: byte %#04x is no H4 packet type: the client's pending bytes are dropped\n", bridge_buf[at]);
			at = bridge_have;
			break;
		}
		if (len == 0) {
			break;
		}
		bridge_buf[at] == H4_CMD ? bridge_count(bridge_buf + at, (uint32_t)len) : (void)0;
		if (!answered_here(bridge_buf + at)) {
			bridge_errors += !link_send(bridge_buf + at, (uint32_t)len);
		}
		at += (uint32_t)len;
	}
	memmove(bridge_buf, bridge_buf + at, bridge_have - at);
	bridge_have -= at;
}

static errno_t
bridge_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *info, mbuf_t m, __unused int flags)
{
	errno_t error = link_available();

	lck_mtx_lock(&bridge_lock);
	for (mbuf_t part = m; error == 0 && part != NULL; part = mbuf_next(part)) {
		size_t n = mbuf_len(part);

		if (bridge_have + n > sizeof(bridge_buf)) {
			bridge_errors++;
			bridge_have = 0;
			error = EMSGSIZE;
			break;
		}
		memcpy(bridge_buf + bridge_have, mbuf_data(part), n);
		bridge_have += (uint32_t)n;
	}
	error == 0 ? bridge_packets() : (void)0;
	lck_mtx_unlock(&bridge_lock);
	mbuf_freem(m);
	return error;
}

static errno_t
bridge_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, void **info)
{
	if (bridge_connected) {
		return EBUSY;
	}
	lck_mtx_lock(&bt_lock);
	ring_head = ring_tail;          /* nothing stale for the new client */
	bridge_unit = sac->sc_unit;
	bridge_connected = true;
	lck_mtx_unlock(&bt_lock);
	bridge_have = 0;
	*info = NULL;
	printf("bt: bridge: client connected (unit %u)\n", sac->sc_unit);
	return 0;
}

static errno_t
bridge_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *info)
{
	lck_mtx_lock(&bt_lock);
	bridge_connected = false;
	lck_mtx_unlock(&bt_lock);
	printf("bt: bridge: client of unit %u gone\n", unit);
	return 0;
}

static errno_t
bridge_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *info, int opt, void *data, size_t *len)
{
	struct user_termios t;

	if ((uint32_t)opt != TIOCGETA_64_OPT) {
		printf("bt: bridge: getsockopt %#x refused\n", opt);
		return ENOPROTOOPT;
	}
	if (data == NULL || *len < sizeof(t)) {
		*len = sizeof(t);
		return data == NULL ? 0 : EINVAL;
	}
	bzero(&t, sizeof(t));
	t.c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
	t.c_ispeed = t.c_ospeed = baud;
	memcpy(data, &t, sizeof(t));
	*len = sizeof(t);
	return 0;
}

static errno_t
bridge_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *info, int opt, void *data, size_t len)
{
	struct user_termios t;

	if ((uint32_t)opt == TIOCSETA_64_OPT && data != NULL && len >= sizeof(t)) {
		memcpy(&t, data, sizeof(t));
		printf("bt: bridge: TIOCSETA ignored (asks %llu baud, cflag %#llx; the link stays at %u)\n", t.c_ospeed, t.c_cflag, baud);
		return 0;
	}
	printf("bt: bridge: setsockopt %#x (%lu bytes) ignored\n", opt, (unsigned long)len);
	return 0;
}

static void
bridge_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	_Static_assert(sizeof(struct user_termios) == 72, "bluetoothd's termios is 72 bytes");
	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = 0;              /* datagram, any user (bluetoothd runs as mobile) */
	reg.ctl_sendsize = 16384;
	reg.ctl_recvsize = 65536;
	reg.ctl_connect = bridge_connect;
	reg.ctl_disconnect = bridge_disconnect;
	reg.ctl_send = bridge_send;
	reg.ctl_setopt = bridge_setopt;
	reg.ctl_getopt = bridge_getopt;
	error = ctl_register(&reg, &bridge_ref);
	printf("bt: bridge: kern_ctl com.apple.uart.bluetooth %s (%d)\n", error == 0 ? "registered" : "NOT REGISTERED", error);
}

/* packets the poller hands to a connected client (never with bt_lock held: the socket has its own locks) */
static void
bridge_flush(void)
{
	static uint8_t packet[H4_MAX];

	for (int i = 0; i < 64; i++) {
		uint32_t n = 0, unit = 0;

		lck_mtx_lock(&bt_lock);
		if (bridge_connected) {
			n = ring_get(packet, sizeof(packet));
			unit = bridge_unit;
		}
		lck_mtx_unlock(&bt_lock);
		if (n == 0) {
			return;
		}
		if (ctl_enqueuedata(bridge_ref, unit, packet, n, CTL_DATA_EOR) == 0) {
			bridge_out++;
		} else {
			bridge_drops++;
		}
	}
}

SYSCTL_DECL(_kern_hakios);

static int
sysctl_bt_status SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[512];

	snprintf(text, sizeof(text), "level %u state %s%s%s chip product %#x soc %#x rom %#x patch %#x firmware %s build %s "
	    "baud %u bdaddr %s ibs %u in %u out %u errors %u dropped %u scans %u found %u bridge connected %u commands %u "
	    "answered_here %u to_client %u client_drops %u client_errors %u", level, state, failed_at[0] ? " at " : "",
	    failed_at, chip.product, chip.soc, chip.rom, chip.patch, fw_text[0] ? fw_text : "none",
	    build_label[0] ? build_label : "?", baud, bdaddr_text, ibs_on, packets_in, packets_out, errors, dropped, scans_done,
	    seen_count, bridge_connected, bridge_cmds, bridge_local, bridge_out, bridge_drops, bridge_errors);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

static void
scan_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t secs = (uint32_t)(uintptr_t)parameter;

	lck_mtx_lock(&bt_cmd_lock);
	kernel_owns = true;
	printf("bt: bt_scan: %u s (the link is the kernel's meanwhile: user writes get EBUSY)\n", secs);
	if (!scan_le(secs)) {
		printf("bt: bt_scan FAILED (%s)\n", failed_at);
	}
	kernel_owns = false;
	scan_running = false;
	lck_mtx_unlock(&bt_cmd_lock);
	thread_terminate(current_thread());
}

static int
sysctl_bt_scan SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = (int)scan_found, error = SYSCTL_OUT(req, &value, sizeof(value));
	thread_t thread;

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	error = SYSCTL_IN(req, &value, sizeof(value));
	if (error != 0) {
		return error;
	}
	if (value < 1 || value > 60) {
		return EINVAL;
	}
	if (level < 3 || strcmp(state, "failed") == 0 || !chip.known || scan_running) {
		return scan_running ? EBUSY : ENODEV;
	}
	scan_running = true;
	if (kernel_thread_start(scan_thread, (void *)(uintptr_t)value, &thread) != KERN_SUCCESS) {
		scan_running = false;
		return ENOMEM;
	}
	thread_deallocate(thread);
	return 0;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, bt_status, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctl_bt_status, "A", "the WCN6750 Bluetooth driver's state");
SYSCTL_PROC(_kern_hakios, OID_AUTO, bt_scan, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    sysctl_bt_scan, "I", "write N (1..60): an LE scan of N seconds, logged (bt=3 or 4); reads the devices it found");

/* the RX side while serving: 1 ms after recent traffic, 10 ms when quiet, 20 ms after 10 s of silence */
__attribute__((noreturn)) static void
poll_forever(void)
{
	for (;;) {
		uint64_t quiet = now_ms() - last_activity_ms;
		struct timespec ts = { 0, (long)(quiet < 200 ? 1 : quiet < 10000 ? 10 : 20) * 1000000L };

		if (!kernel_owns) {
			pump();
		}
		bridge_flush();
		lck_mtx_lock(&bt_lock);
		(void)msleep(&last_activity_ms, &bt_lock, PZERO, "bt-rx", &ts);
		lck_mtx_unlock(&bt_lock);
	}
}

__attribute__((noreturn)) static void
serve(void)
{
	int major = cdevsw_add(-1, &bt_cdevsw);

	step("serving user space: /dev/hakios-bt and /dev/uart.bluetooth (raw H4), kern_ctl com.apple.uart.bluetooth");
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-bt") == NULL ||
	    devfs_make_node(makedev(major, 1), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth") == NULL) {
		printf("bt: the device nodes cannot be made (major %d)\n", major);
	}
	bridge_register();
	lck_mtx_lock(&bt_lock);
	ring_head = ring_tail;
	serving = true;
	lck_mtx_unlock(&bt_lock);
	state = "serving";
	printf("bt: serving: H4 packets both ways, the RX FIFO polled\n");
	poll_forever();
}

/* ---- the look (bt=1) and the bring-up --------------------------------------------------------------------------- */

static void
look(void)
{
	step("a look, reading only: pins, clocks, PLL, supplies, then the serial engine if its clocks are on");
	show_pins();
	show_clocks();
	show_supplies();
	if (!se_clocks_on()) {
		printf("bt: SE7's clocks are not all on: its registers are not read (an access would hang the bus)\n");
		return;
	}
	sampling = rd(QUP_HW_VER) >= QUP_SE_VERSION_2_5 ? 16 : 32;
	show_se();
}

static bool
bring_up_uart(void)
{
	uint64_t hz;

	if (!supplies_on()) {
		return false;
	}
	/* the sampling needs the wrapper's version, which needs its clocks: a first pass at the 3 Mbit/s rate for 16 */
	hz = (uint64_t)FAST_BAUD * 16;
	if (!clocks_on(hz)) {
		return false;
	}
	sampling = rd(QUP_HW_VER) >= QUP_SE_VERSION_2_5 ? 16 : 32;
	printf("bt: QUP version %#x: oversampling %u (qcom_geni_serial.c: halved from 32 for 2.5 and later)\n", rd(QUP_HW_VER),
	    sampling);
	if (sampling != 16 && !set_se_rate((uint64_t)FAST_BAUD * sampling)) {
		return false;
	}
	return take_uart_pins() && uart_init() && chip_on();
}

static void
bt_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool ok;

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	qup = ml_io_map(QUP_PHYS, 0x2000);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	look();
	if (level < 2) {
		printf("bt: look done\n");
		thread_terminate(current_thread());
		return;
	}
	lck_mtx_lock(&bt_cmd_lock);
	kernel_owns = true;
	ok = bring_up_uart() && read_version();
	ok = ok && (level != 2 || rom_reset_and_version());
	ok = ok && (level < 3 || qca_setup());
	ok = ok && (level < 3 || scan_le(scan_secs));
	ok = ok && (level < 3 || !inquiry_wanted || inquiry());
	kernel_owns = false;
	lck_mtx_unlock(&bt_cmd_lock);
	if (!ok) {
		give_up();
		thread_terminate(current_thread());
		return;
	}
	state = level == 2 ? "rom" : "ready";
	printf("bt: level %u done: %s\n", level, level == 2 ? "the chip answers in ROM (left powered)" : "set up, discovery done");
	if (level >= 4) {
		serve();
	}
	thread_terminate(current_thread());
}

void
dlisa_bt_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("bt", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = MIN(level, 4);
	PE_parse_boot_argn("bt_ibs", &ibs_wanted, sizeof(ibs_wanted));
	PE_parse_boot_argn("bt_fast", &fast_wanted, sizeof(fast_wanted));
	PE_parse_boot_argn("bt_inquiry", &inquiry_wanted, sizeof(inquiry_wanted));
	PE_parse_boot_argn("bt_scan_secs", &scan_secs, sizeof(scan_secs));
	scan_secs = MAX(1, MIN(scan_secs, 60));
	printf("bt: level %u (WCN6750 on QUP0 SE7, BT_EN gpio%u; in-band sleep %u, 3 Mbit/s %u, inquiry %u, scan %u s)\n", level,
	    PIN_BT_EN, ibs_wanted, fast_wanted, inquiry_wanted, scan_secs);
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
