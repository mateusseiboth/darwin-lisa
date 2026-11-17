/*
 * darwin-lisa: the panel's brightness, through DCS commands on the DSI link the boot firmware left running.
 *
 * The panel (CSOT VTDR6130, Xiaomi's "k9d 36 02 0a cmd mode dsc") runs in DSI command mode: ABL brought it up and the
 * DPU sends it frames on its tearing signal. Its brightness is the DCS register 0x51 (12 bits, high byte first,
 * dlisa_dcs.h); 0x52 reads it back. A command goes out through the DSI host's command DMA, as Linux's msm driver sends
 * them (dsi_host.c): the packet in memory, DMA_BASE/DMA_LEN, a software trigger, CMD_DMA_DONE polled in INTR_CTRL (the
 * vendor driver's polling mode; nothing here takes the DSI interrupt). The host waits for the frame the DPU is sending
 * to end before it sends the packet (TRIG_CTRL.BLOCK_DMA_WITHIN_FRAME, which Linux sets on every 6G host from v1.2).
 *
 * Registers: DSI0 at 0x0ae94000 (SM7325 = sc7280, DSI 6G v2.5.0). The offsets are Linux's dsi.xml plus 4: on a 6G host
 * offset 0 holds the hardware version and every other register sits 4 bytes further (DSI_6G_REG_SHIFT).
 *
 * Boot-args (nothing is read, written or sent without bright=):
 *   bright=1          the probe (registers to the log) and the sysctls; bright=2 only the probe, nothing sent
 *   bright_level=N    a level (1..2047) sent once at start
 *   bright_seq=L@S,.. levels at S seconds after the framebuffer started, each read back (0x52); "r@S" only reads
 *   bright_lp=0       packets in high speed (default 1: low power, as Xiaomi sends 0x51 to this panel)
 *   bright_min=N bright_max=N   the range sent (default 1..2047; never above 2047, the panel's high brightness mode)
 *   bright_buf=PHYS   the packet from that physical page (mapped uncached) instead of a buffer of the kernel's
 *   bright_fifo=1     the packet through the test pattern generator's DMA FIFO (no memory: the vendor driver's
 *                     kickoff_fifo_command; offsets from the vendor's dsi_ctrl_reg.h, never run here)
 *   bright_dim=1      0x53 0x28 once at start (the panel's own smooth dimming); default: 0x53 left as the firmware set it
 *   bright_dsi=PHYS   the DSI host's registers (default: DSI0 of sc7280 when hakios_kver says so)
 *   bright_force=1    go on when the host's version is not 2.5
 *   bright_log=N      log the first N level changes and every 64th after (default 24)
 *
 * sysctls: kern.hakios.brightness (read: the level last sent, -1 none; write: a level, sent at once),
 * kern.hakios.brightness_read (a 0x52 read now: the level, or a negative error), kern.hakios.brightness_status.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/errno.h>

extern "C" {
#include "dlisa_dcs.h"
}

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void dlisa_brightness_init(void);

#define DSI0_SC7280             0x0ae94000UL

/* DSI 6G register offsets (Linux dsi.xml + 4), and the vendor's for the test pattern FIFO */
#define DSI_HW_VERSION          0x000
#define DSI_CTRL                0x004
#define DSI_STATUS0             0x008
#define DSI_FIFO_STATUS         0x00c
#define DSI_CMD_DMA_CTRL        0x03c
#define DSI_DMA_BASE            0x048
#define DSI_DMA_LEN             0x04c
#define DSI_ACK_ERR_STATUS      0x068
#define DSI_RDBK_DATA(i)        (0x06c + 4 * (i))
#define DSI_TRIG_CTRL           0x084
#define DSI_TRIG_DMA            0x090
#define DSI_LANE_STATUS         0x0a8
#define DSI_INTR_CTRL           0x110
#define DSI_CLK_STATUS          0x120
#define DSI_TPG_CTRL            0x15c
#define DSI_TPG_CMD_DMA_INIT    0x17c           /* vendor: DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL (bright_fifo=1 only) */
#define DSI_RDBK_DATA_CTRL      0x1d4

#define CTRL_ENABLE             (1u << 0)
#define CTRL_CMD_MODE_EN        (1u << 2)
#define STATUS0_DMA_BUSY        (1u << 1)
#define STATUS0_MDP_BUSY        (1u << 2)       /* a frame of the DPU going out */
#define CMD_DMA_LOW_POWER       (1u << 26)
#define CMD_DMA_EMBEDDED        (1u << 28)      /* Linux: FROM_FRAME_BUFFER; the vendor: embedded mode */
#define TRIG_DMA_FIELD          0x7u
#define TRIG_DMA_SW             0x4u
#define TRIG_BLOCK_DMA_IN_FRAME (1u << 12)
#define INTR_CMD_DMA_DONE       (1u << 0)
#define INTR_STATUS_BITS        ((1u << 0) | (1u << 8) | (1u << 16) | (1u << 20) | (1u << 24))
#define TPG_FIFO_MODE           ((1u << 1) | (1u << 2) | (3u << 16))

#define DMA_DONE_US             50000           /* Linux waits 200 ms; a 3-word packet takes microseconds */
#define BUSY_WAIT_US            20000

static TUNABLE(uint32_t, gBright, "bright", 0);
static TUNABLE(uint32_t, gBrightLevel, "bright_level", 0);
static TUNABLE(uint32_t, gBrightLP, "bright_lp", 1);
static TUNABLE(uint32_t, gBrightMin, "bright_min", 1);
static TUNABLE(uint32_t, gBrightMax, "bright_max", DCS_LEVEL_NORMAL_MAX);
static TUNABLE(uint64_t, gBrightBuf, "bright_buf", 0);
static TUNABLE(uint32_t, gBrightFifo, "bright_fifo", 0);
static TUNABLE(uint32_t, gBrightDim, "bright_dim", 0);
static TUNABLE(uint64_t, gBrightDsi, "bright_dsi", 0);
static TUNABLE(uint32_t, gBrightForce, "bright_force", 0);
static TUNABLE(uint32_t, gBrightLog, "bright_log", 24);

namespace {

struct Panel {
	IOLock *                        lock;
	volatile uint8_t *              dsi;            /* the host's registers */
	IOBufferMemoryDescriptor *      memory;         /* the packet's buffer (the kernel's) ... */
	volatile uint32_t *             buffer;         /* ... or bright_buf's page; NULL with bright_fifo */
	uint64_t                        physical;
	const char *                    path;
	bool                            ready;          /* the probe found a host in command mode */
	bool                            broken;         /* a DMA never finished: nothing more is sent this boot */
	int                             level;          /* last level sent, -1 none */
	int                             readBack;       /* last 0x52 answer, or a negative error */
	uint32_t                        sent, failed, timeouts, changes;
	uint64_t                        started;        /* mach_absolute_time of dlisa_brightness_init */
};

Panel gPanel;

inline uint32_t
readRegister(uint32_t offset)
{
	return *(volatile uint32_t *)(gPanel.dsi + offset);
}

inline void
writeRegister(uint32_t offset, uint32_t value)
{
	__asm__ volatile ("dsb st" ::: "memory");
	*(volatile uint32_t *)(gPanel.dsi + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

inline uint64_t
microsecondsSince(uint64_t then)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - then, &ns);
	return ns / 1000;
}

/* spin until (register & mask) == want, at most `us` microseconds */
bool
waitFor(uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t began = mach_absolute_time();

	while ((readRegister(offset) & mask) != want) {
		if (microsecondsSince(began) > us) {
			return false;
		}
		IODelay(5);
	}
	return true;
}

void
logRegisters(const char * when)
{
	IOLog("brightness: %s: ctrl %#x status0 %#x fifo %#x cmd_dma_ctrl %#x dma_base %#x dma_len %#x trig %#x intr %#x "
	    "ack_err %#x lanes %#x clk %#x tpg %#x\n", when, readRegister(DSI_CTRL), readRegister(DSI_STATUS0),
	    readRegister(DSI_FIFO_STATUS), readRegister(DSI_CMD_DMA_CTRL), readRegister(DSI_DMA_BASE),
	    readRegister(DSI_DMA_LEN), readRegister(DSI_TRIG_CTRL), readRegister(DSI_INTR_CTRL),
	    readRegister(DSI_ACK_ERR_STATUS), readRegister(DSI_LANE_STATUS), readRegister(DSI_CLK_STATUS),
	    readRegister(DSI_TPG_CTRL));
}

void
clearDmaDone(void)
{
	writeRegister(DSI_INTR_CTRL, (readRegister(DSI_INTR_CTRL) & ~INTR_STATUS_BITS) | INTR_CMD_DMA_DONE);
}

/* the packet into the buffer the DMA reads (memory paths) */
void
storePacket(const uint32_t * words, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		gPanel.buffer[i] = words[i];
	}
	if (gPanel.memory != NULL) {
		gPanel.memory->performOperation(kIOMemoryIncoherentIOStore, 0, count * sizeof(uint32_t));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	writeRegister(DSI_DMA_BASE, (uint32_t) gPanel.physical);
	writeRegister(DSI_DMA_LEN, count * sizeof(uint32_t));
}

/* the packet into the test pattern generator's FIFO (bright_fifo=1): the vendor's kickoff_fifo_command */
void
fifoPacket(const uint32_t * words, unsigned count)
{
	writeRegister(DSI_TPG_CTRL, TPG_FIFO_MODE);
	for (unsigned i = 0; i < count; i++) {
		writeRegister(DSI_TPG_CMD_DMA_INIT, words[i]);
	}
	if (count & 1) {
		writeRegister(DSI_TPG_CMD_DMA_INIT, 0);
	}
	writeRegister(DSI_DMA_LEN, count * sizeof(uint32_t));
}

/* One packet out (lock held). True when the host said CMD_DMA_DONE. */
bool
sendPacket(const uint32_t * words, unsigned count)
{
	uint32_t dmaCtrl, wanted;
	uint64_t began;
	bool done;

	if (!gPanel.ready || gPanel.broken || count == 0) {
		return false;
	}
	if ((readRegister(DSI_CTRL) & (CTRL_ENABLE | CTRL_CMD_MODE_EN)) != (CTRL_ENABLE | CTRL_CMD_MODE_EN)) {
		gPanel.failed++;
		IOLog("brightness: the DSI host left command mode (ctrl %#x): not sent\n", readRegister(DSI_CTRL));
		return false;
	}
	/*
	 * Between two frames: the DPU's frames go out through this same host (command mode, sent on the panel's tearing
	 * signal), so the packet waits until no frame is being sent (and no earlier packet). BLOCK_DMA_WITHIN_FRAME makes the
	 * host itself hold a packet triggered inside a frame; this wait keeps the trigger out of one in the first place.
	 */
	if (!waitFor(DSI_STATUS0, STATUS0_DMA_BUSY | STATUS0_MDP_BUSY, 0, BUSY_WAIT_US)) {
		gPanel.failed++;
		IOLog("brightness: the host stays busy with a frame or a packet (status0 %#x): not sent\n", readRegister(DSI_STATUS0));
		return false;
	}
	dmaCtrl = readRegister(DSI_CMD_DMA_CTRL);
	wanted = (dmaCtrl | CMD_DMA_EMBEDDED) & ~CMD_DMA_LOW_POWER;
	wanted |= gBrightLP != 0 ? CMD_DMA_LOW_POWER : 0;
	writeRegister(DSI_CMD_DMA_CTRL, wanted);
	if (gPanel.buffer != NULL) {
		storePacket(words, count);
	} else {
		fifoPacket(words, count);
	}
	clearDmaDone();
	began = mach_absolute_time();
	writeRegister(DSI_TRIG_DMA, 1);
	if (gPanel.buffer == NULL) {
		writeRegister(DSI_TPG_CTRL, 0);         /* the vendor resets the FIFO right after the trigger */
	}
	done = waitFor(DSI_INTR_CTRL, INTR_CMD_DMA_DONE, INTR_CMD_DMA_DONE, DMA_DONE_US);
	clearDmaDone();
	writeRegister(DSI_CMD_DMA_CTRL, dmaCtrl);
	if (!done) {
		gPanel.timeouts++;
		gPanel.broken = true;
		logRegisters("the command DMA never finished; nothing more is sent this boot");
		return false;
	}
	gPanel.sent++;
	if (gPanel.sent <= 4) {
		IOLog("brightness: packet %#x%s sent in %llu us\n", words[0], count > 1 ? " ..." : "",
		    microsecondsSince(began));
	}
	return true;
}

/* 0x52 (lock held): the level, or a negative error */
int
readLevel(void)
{
	uint32_t words[1], rdbk[4], ack;
	uint8_t bytes[2] = { 0, 0 };
	int count;

	writeRegister(DSI_RDBK_DATA_CTRL, 1);
	writeRegister(DSI_RDBK_DATA_CTRL, 0);
	dcsMaxReturnPacket(words, 1, 2);
	if (!sendPacket(words, 1)) {
		return -5;
	}
	dcsReadPacket(words, 1, DCS_GET_BRIGHTNESS);
	if (!sendPacket(words, 1)) {
		return -5;
	}
	for (unsigned i = 0; i < 4; i++) {
		rdbk[i] = readRegister(DSI_RDBK_DATA(i));
	}
	ack = readRegister(DSI_ACK_ERR_STATUS);
	if (ack != 0) {
		writeRegister(DSI_ACK_ERR_STATUS, ack);
		writeRegister(DSI_ACK_ERR_STATUS, 0);
	}
	count = dcsParseRead(rdbk, bytes, 2);
	IOLog("brightness: read 0x52: rdbk %08x %08x %08x %08x ack_err %#x -> %d byte(s) %02x %02x\n", rdbk[0], rdbk[1],
	    rdbk[2], rdbk[3], ack, count, bytes[0], bytes[1]);
	if (count < 0) {
		return count;
	}
	return count == 2 ? (bytes[0] << 8 | bytes[1]) : bytes[0];
}

bool
isLogged(uint32_t change)
{
	return change <= gBrightLog || (change & 63) == 0;
}

/* a level to the panel (lock held) */
int
setLevel(int asked, bool always_log)
{
	uint32_t words[2];
	uint16_t level = dcsClampLevel(asked, (int) gBrightMin, (int) gBrightMax);

	dcsBrightnessPacket(words, 2, level);
	if (!sendPacket(words, 2)) {
		return EIO;
	}
	gPanel.level = level;
	gPanel.changes++;
	if (always_log || isLogged(gPanel.changes)) {
		IOLog("brightness: dcs 51 %02x %02x (level %u of %u, asked %d; change %u)\n", level >> 8, level & 0xff, level,
		    DCS_LEVEL_NORMAL_MAX, asked, gPanel.changes);
	}
	return 0;
}

uint32_t
parseNumber(const char ** text)
{
	uint32_t value = 0, base = 10;
	const char * p = *text;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	}
	for (;; p++) {
		uint32_t digit = *p >= '0' && *p <= '9' ? (uint32_t) (*p - '0') :
		    base == 16 && *p >= 'a' && *p <= 'f' ? (uint32_t) (*p - 'a' + 10) :
		    base == 16 && *p >= 'A' && *p <= 'F' ? (uint32_t) (*p - 'A' + 10) : 99;
		if (digit >= base) {
			break;
		}
		value = value * base + digit;
	}
	*text = p;
	return value;
}

void
sleepUntil(uint32_t seconds)
{
	uint64_t elapsed = microsecondsSince(gPanel.started) / 1000;

	if (elapsed < (uint64_t) seconds * 1000) {
		IOSleep((uint32_t) ((uint64_t) seconds * 1000 - elapsed));
	}
}

/* bright_seq=L@S,...: each step a level (or "r", only a read) at S seconds, then the read-back */
void
runSequence(const char * text)
{
	while (*text != '\0') {
		bool readOnly = *text == 'r';
		uint32_t level = readOnly ? 0 : parseNumber(&text);
		uint32_t seconds;

		text += readOnly ? 1 : 0;
		if (*text != '@') {
			IOLog("brightness: bright_seq: '%c' where an @ was expected: the rest is left out\n", *text);
			return;
		}
		text++;
		seconds = parseNumber(&text);
		sleepUntil(seconds);
		IOLockLock(gPanel.lock);
		if (!readOnly) {
			setLevel((int) level, true);
			IOSleep(20);
		}
		gPanel.readBack = readLevel();
		IOLog("brightness: bright_seq at %u s: %s %u, read back %d\n", seconds, readOnly ? "read" : "level", level,
		    gPanel.readBack);
		IOLockUnlock(gPanel.lock);
		text += *text == ',' ? 1 : 0;
	}
}

vm_offset_t
hostRegisters(void)
{
	char version[128] = "";
	uint64_t physical = gBrightDsi;

	if (physical == 0 && PE_parse_boot_argn("hakios_kver", version, sizeof(version)) &&
	    strnstr(version, "-sc7280-", sizeof(version)) != NULL) {
		physical = DSI0_SC7280;
	}
	return physical != 0 ? ml_io_map((vm_offset_t) (physical & ~0x3fffULL), 0x4000) + (vm_offset_t) (physical & 0x3fffULL) : 0;
}

/* where the packet comes from: the kernel's memory, bright_buf's page, or the FIFO */
bool
choosePath(void)
{
	if (gBrightFifo != 0) {
		gPanel.path = "fifo";
		return true;
	}
	if (gBrightBuf != 0) {
		gPanel.physical = gBrightBuf & ~0x3fffULL;
		gPanel.buffer = (volatile uint32_t *) ml_io_map((vm_offset_t) gPanel.physical, 0x4000);
		gPanel.path = "bright_buf";
		return gPanel.buffer != NULL;
	}
	/* below 4 GB (DMA_BASE is 32 bits), one page, physically contiguous */
	gPanel.memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionOut | kIOMemoryPhysicallyContiguous, 0x4000, 0x00000000ffffc000ULL);
	if (gPanel.memory == NULL) {
		return false;
	}
	gPanel.buffer = (volatile uint32_t *) gPanel.memory->getBytesNoCopy();
	gPanel.physical = gPanel.memory->getPhysicalSegment(0, NULL);
	gPanel.path = "dma";
	return gPanel.buffer != NULL && gPanel.physical != 0 && gPanel.physical < (1ULL << 32);
}

/* the host's state, and whether a packet can be sent at all */
bool
probe(void)
{
	uint32_t version = readRegister(DSI_HW_VERSION), ctrl = readRegister(DSI_CTRL), trig;

	IOLog("brightness: DSI host version %#x (major %u minor %u)\n", version, version >> 28, (version >> 16) & 0xfff);
	logRegisters("as the firmware left it");
	if ((version >> 16) != 0x2005 && gBrightForce == 0) {
		IOLog("brightness: not the DSI 2.5 host of sc7280: nothing is sent (bright_force=1 goes on)\n");
		return false;
	}
	if ((ctrl & (CTRL_ENABLE | CTRL_CMD_MODE_EN)) != (CTRL_ENABLE | CTRL_CMD_MODE_EN)) {
		IOLog("brightness: the host is not in command mode (ctrl %#x): nothing is sent\n", ctrl);
		return false;
	}
	trig = readRegister(DSI_TRIG_CTRL);
	if ((trig & TRIG_DMA_FIELD) != TRIG_DMA_SW || !(trig & TRIG_BLOCK_DMA_IN_FRAME)) {
		uint32_t wanted = (trig & ~TRIG_DMA_FIELD) | TRIG_DMA_SW | TRIG_BLOCK_DMA_IN_FRAME;

		writeRegister(DSI_TRIG_CTRL, wanted);
		IOLog("brightness: TRIG_CTRL %#x -> %#x (software DMA trigger, DMA held while a frame goes out)\n", trig,
		    readRegister(DSI_TRIG_CTRL));
	}
	return true;
}

void
brightnessMain(__unused void * argument, __unused wait_result_t result)
{
	char sequence[256] = "";

	gPanel.dsi = (volatile uint8_t *) hostRegisters();
	if (gPanel.dsi == NULL) {
		IOLog("brightness: no DSI host known for this SoC (bright_dsi=): nothing is sent\n");
		return;
	}
	gPanel.ready = probe() && gBright == 1 && choosePath();
	if (!gPanel.ready) {
		IOLog("brightness: %s\n", gBright == 2 ? "bright=2: the probe only" : "nothing will be sent");
		return;
	}
	IOLog("brightness: packets from %s (physical %#llx), %s; levels %u..%u\n", gPanel.path, gPanel.physical,
	    gBrightLP ? "low power" : "high speed", gBrightMin, gBrightMax < DCS_LEVEL_NORMAL_MAX ? gBrightMax : DCS_LEVEL_NORMAL_MAX);
	IOLockLock(gPanel.lock);
	gPanel.readBack = readLevel();
	IOLog("brightness: the firmware's level: %d\n", gPanel.readBack);
	if (gBrightDim != 0) {
		uint32_t words[1];

		dcsShortWrite1Packet(words, 1, DCS_WRITE_CONTROL, 0x28);
		IOLog("brightness: dcs 53 28 (smooth dimming on): %s\n", sendPacket(words, 1) ? "sent" : "NOT sent");
	}
	if (gBrightLevel != 0) {
		setLevel((int) gBrightLevel, true);
	}
	IOLockUnlock(gPanel.lock);
	if (PE_parse_boot_arg_str("bright_seq", sequence, sizeof(sequence))) {
		runSequence(sequence);
	}
}

int
sysctlBrightness SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = gPanel.level, error = SYSCTL_OUT(req, &value, sizeof(value));

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	error = SYSCTL_IN(req, &value, sizeof(value));
	if (error != 0) {
		return error;
	}
	if (!gPanel.ready || gPanel.broken) {
		return ENODEV;
	}
	IOLockLock(gPanel.lock);
	error = value == gPanel.level ? 0 : setLevel(value, false);
	IOLockUnlock(gPanel.lock);
	return error;
}

int
sysctlBrightnessRead SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = DCS_READ_NOTHING;

	if (gPanel.ready && !gPanel.broken) {
		IOLockLock(gPanel.lock);
		value = gPanel.readBack = readLevel();
		IOLockUnlock(gPanel.lock);
	}
	return SYSCTL_OUT(req, &value, sizeof(value));
}

int
sysctlBrightnessStatus SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[200];

	snprintf(text, sizeof(text), "ready %d broken %d level %d read %d sent %u failed %u timeouts %u changes %u path %s lp %u",
	    gPanel.ready, gPanel.broken, gPanel.level, gPanel.readBack, gPanel.sent, gPanel.failed, gPanel.timeouts,
	    gPanel.changes, gPanel.path != NULL ? gPanel.path : "none", gBrightLP);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

} /* namespace */

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    sysctlBrightness, "I", "the panel's level (DCS 0x51), 1..2047; reads the last sent, -1 none (bright=1)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness_read, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctlBrightnessRead, "I", "the panel's level as it answers DCS 0x52 now, or a negative error");
SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness_status, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctlBrightnessStatus, "A", "the brightness driver's state");

/* from IOMobileFramebuffer::start (display=1); inert without bright= */
extern "C" void
dlisa_brightness_init(void)
{
	thread_t thread;

	if (gBright == 0 || gPanel.lock != NULL) {
		return;
	}
	gPanel.level = -1;
	gPanel.readBack = DCS_READ_NOTHING;
	gPanel.lock = IOLockAlloc();
	gPanel.started = mach_absolute_time();
	if (kernel_thread_start(brightnessMain, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "the panel's brightness");
		thread_deallocate(thread);
	}
}
