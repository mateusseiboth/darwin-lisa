/*
 * darwin-lisa: the speakers of the Xiaomi 11 Lite 5G NE (lisa, SM7325), through the audio DSP's APR services, on
 * xnu-12377. HakiOS's drivers/audio/hakios.sound.c (proven on redwood, the same SoC and DSP family) with lisa's
 * interface, amplifiers and numbers. Nothing here happens without boot-arg sound=N, and nothing before the DSP's audio
 * services are up: hakios.adsp.c (adsp=1) calls hakios_sound_dsp_up() once step 11 has heard "THE AUDIO SERVICES ARE
 * UP", hakios_sound_dsp_down() before it shuts the DSP down, and hakios_sound_quiesce() on a panic's way out. Those
 * three are defined here (option HAKIOS_SOUND, set by audio-wire.py beside DLISA_SOUND, drops hakios.stubs.c's).
 *
 * The path, as pmOS's kernel plays lisa's speakers (sm7325-xiaomi-lisa.dtb, sound/soc/qcom/sm8250.c, qdsp6/ of
 * Linux 7.2.9, UCM Xiaomi/lisa: "PRI_MI2S_RX Audio Mixer MultiMedia1"):
 *   ASM   a stream (session 1, stream 1) opened for writing 16-bit linear PCM, no processing (topology 0x10c68), the
 *         samples in memory of ours that the DSP reads through the application processors' SMMU as stream 0x1801
 *         (pmOS: q6asm-dais iommus <&apps_smmu 0x1801 0>; q6asm-dai.c puts the stream's low four bits, 1, in bits 32
 *         to 35 of every address the DSP is given), mapped with ASM_CMD_SHARED_MEM_MAP_REGIONS
 *   ADM   a device path (COPP) for playback to the port, no processing (topology 0x10312), 2 channels of 16 bits at
 *         48 kHz, and session 1 routed to it (matrix 0, audio RX)
 *   AFE   the port PRIMARY_MI2S_RX (0x1000; q6afedai dai@16 with qcom,sd-lines <1>): the bit clock
 *         Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT (0x100) at 1.536 MHz, attribute COUPLE_NO (1), root default (sm8250.c
 *         MI2S_BCLK_RATE, its startup for PRIMARY_MI2S_RX, read from pmOS's snd-soc-sm8250.ko: clock 0x100, 0x177000 Hz,
 *         cpu fmt BP_FP, codec fmt I2S | BC_FC); AFE_PARAM_ID_I2S_CONFIG: 16 bits (be_hw_params_fixup: 48 kHz, 2
 *         channels, S16_LE), channel mode SD1 (2), stereo, word select internal, 48 kHz, linear PCM; then started
 *   pins  GPIO 97 mi2s0_sck, 98 mi2s0_data0, 99 mi2s0_data1, 100 mi2s0_ws: function 1 each (pinctrl-sc7280.c), 8 mA,
 *         no pull, output high (pmOS's pri-mi2s-*-active states); put back as found afterwards
 *   amps  two TFA9873 on I2C (dlisa.tfa9873.c), each taking its slot: 0x34 the left (top), 0x35 the right (bottom)
 *
 *   sound=1  the port: pins, bit clock, I2S config, port started; whether the bit clock and the word select toggle at
 *            the pins is looked at; held sound_secs (default 3, at most 10); everything stopped and put back
 *   sound=2  ... and a stream: the SMMU context, the DSP's view of our periods, ADM path, ASM stream, routed, run, and
 *            a test tone (1 kHz, -20 dBFS, both channels) for sound_secs (at most 3). The DSP's "write done" events
 *            say whether it takes 48000 frames a second. The amplifiers are only looked at (I2C reads), never written
 *   sound=3  ... and the amplifiers: set up as pmOS's tfa9872.c does once the clock runs, powered up, unmuted, the
 *            tone faded in over 100 ms and out over 100 ms at sound_atten dB under -20 dBFS (default 12: -32 dBFS),
 *            muted and powered down at the end. Audible. kern.hakios.sound_tone=MS plays it again (200..3000 ms)
 *   sound=4  serve Darwin: /dev/hakios-audio (hakios.audio.c's sink: 16-bit stereo at 48 kHz) to the speakers until
 *            the run ends; port and stream kept running (silence when nothing is written); the amplifiers powered up
 *            and faded in when a write is not silence, powered down after 3 s of silence; Darwin's volume control
 *            (HAKIOS_AUDIO_SET_VOLUME) added to sound_atten; muted = amplifiers off. Every sample goes through a DC
 *            blocker (one pole, about 40 Hz), the gain and a clip. kern.hakios.sound_tone=MS puts the test tone in
 *            as if Darwin had written it.
 *
 * Boot-args: sound=1..4 (needs adsp=1), sound_secs=S, sound_atten=DB (6..60, default 12), sound_amps=1|2|3 (1 the top
 * 0x34, 2 the bottom 0x35, default 3), sound_amp_reset=1, sound_dcdc=0, sound_pause=MS (after each announcement of a
 * step not yet seen to work, default 300).
 * Sysctls: kern.hakios.sound_status (string), kern.hakios.sound_tone (RW int, root: ms; reads the tones played).
 *
 * Guards: what is started is stopped in reverse order (amplifiers first, while the clock still runs); every DSP
 * command's answer is checked and an unexpected one ends the level there; the tone is three seconds at most and never
 * louder than -20 dBFS; the amplifiers are looked at every 200 ms while on and both are powered down on anything
 * unexpected; at least 6 dB of attenuation on Darwin's samples. Layouts: q6afe.c, q6adm.c, q6asm.c of Linux and
 * HakiOS's hakios.sound.c; the SMMU: HakiOS's (hakios.wifi.c's way), as dlisa.fastrpc.c does for stream 0x1805.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.audio.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>

#include "dlisa.tfa9873.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern lck_mtx_t hakios_smmu_lock;      /* hakios.platform.c: the FastRPC server and the Wi-Fi take SMMU contexts too */

static LCK_GRP_DECLARE(sound_lock_group, "dlisa-sound");
static LCK_MTX_DECLARE(sound_lock, &sound_lock_group);  /* one sequence at a time: a level, a tone, the take-down */

#define TONE_MAX_MS             3000
#define TONE_MIN_MS             200
#define ATTEN_MIN_DB            6
#define ATTEN_MAX_DB            60

static uint32_t level, seconds = 3, atten_db = 12, pause_ms = 300;
static char last_write[96] = "nothing written";
static char state_text[96] = "off (sound=N)";
static volatile uint32_t tones_played;

/* ---- what is said: to the loader's log as well, as it happens ---------------------------------------- */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
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
state(const char *text)
{
	strlcpy(state_text, text, sizeof(state_text));
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

static void
say_when(const char *what)
{
	uint64_t up = now_us();

	say("sound: %s (%llu.%03llu s after the kernel began)\n", what, up / 1000000, up / 1000 % 1000);
}

/* Time for the log to be read out before something not yet seen to be harmless. */
static void
announce(const char *what)
{
	say("sound: %s\n", what);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	value = rd(b->base + offset);
	say("sound: %s %s (%#x) = %#x\n", b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	say("sound: about to write %s %s at %#x: %#x\n", b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "wrote %#x to %s %s at %#x", value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the MI2S pins --------------------------------------------------------------------------------------------- */

static struct block tlmm = { "pin controller", 0x0f100000, 0x100000, 0 };
#define GPIO_CFG(n)             (0x1000u * (n))         /* [1:0] pull, [5:2] function, [8:6] drive (mA / 2 - 1), [9] output */
#define GPIO_IO(n)              (0x1000u * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_MI2S0               (1u << 2)               /* function 1 on 97..100: mi2s0_sck, _data0, _data1, _ws */
#define CFG_8MA                 (3u << 6)
#define CFG_OUTPUT              (1u << 9)

/* pmOS's pri-mi2s-{bck,din,dout,ws}-active: function mi2s0_*, 8 mA, bias-disable, output-high, every one */
static const struct {
	unsigned pin;
	const char *name;
} mi2s_pins[] = {
	{ 97, "gpio97 (bit clock)" }, { 98, "gpio98 (data 0, unused)" }, { 99, "gpio99 (data 1: the speakers)" },
	{ 100, "gpio100 (word select)" },
};
#define MI2S_PINS               (sizeof(mi2s_pins) / sizeof(mi2s_pins[0]))
static uint32_t found_cfg[MI2S_PINS], found_io[MI2S_PINS];
static bool pins_taken;

static void
pins_on(void)
{
	for (unsigned i = 0; i < MI2S_PINS; i++) {
		found_cfg[i] = get(&tlmm, mi2s_pins[i].name, GPIO_CFG(mi2s_pins[i].pin));
		found_io[i] = get(&tlmm, mi2s_pins[i].name, GPIO_IO(mi2s_pins[i].pin));
	}
	pins_taken = true;
	for (unsigned i = 0; i < MI2S_PINS; i++) {
		put(&tlmm, mi2s_pins[i].name, GPIO_IO(mi2s_pins[i].pin), 2);
		put(&tlmm, mi2s_pins[i].name, GPIO_CFG(mi2s_pins[i].pin), CFG_MI2S0 | CFG_8MA | CFG_OUTPUT);
	}
}

static void
pins_off(void)
{
	if (!pins_taken) {
		return;
	}
	for (unsigned i = 0; i < MI2S_PINS; i++) {
		put(&tlmm, mi2s_pins[i].name, GPIO_CFG(mi2s_pins[i].pin), found_cfg[i]);
		put(&tlmm, mi2s_pins[i].name, GPIO_IO(mi2s_pins[i].pin), found_io[i] & 2);
	}
	pins_taken = false;
}

/* Whether a pin toggles: read many times as fast as it goes (the input bit follows the pad in any function). */
static uint32_t
pin_activity(unsigned pin, const char *name)
{
	uint32_t last = rd(tlmm.base + GPIO_IO(pin)) & 1, reads = 20000, changes = 0, ones = 0;
	uint64_t began = now_us();

	for (uint32_t i = 0; i < reads; i++) {
		uint32_t now = rd(tlmm.base + GPIO_IO(pin)) & 1;

		changes += now != last;
		ones += now;
		last = now;
	}
	say("sound: %s: %u changes and %u ones in %u reads (%llu us)\n", name, changes, ones, reads, now_us() - began);
	return changes;
}

/* ---- APR ------------------------------------------------------------------------------------------- */

#define APR_SVC_AFE             4
#define APR_SVC_ASM             7
#define APR_SVC_ADM             8
#define APR_BASIC_RSP_RESULT    0x000110e8u
#define APR_WAIT_MS             1000

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "an error this does not know";
}

/*
 * One command, its answer waited for: true if the answer is the basic one with status 0 (and nothing else is
 * wanted), or the opcode `wanted' (then in *answer). Anything else is said and is false: the level stops there.
 */
static bool
command(const char *what, uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct hakios_apr_answer *answer)
{
	struct hakios_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	say("sound: > %s (opcode %#x to service %u, port %#x, %u bytes)\n", what, opcode, service, port, length);
	snprintf(last_write, sizeof(last_write), "sent %s (opcode %#x)", what, opcode);
	if (!hakios_adsp_apr(service, port, 0, opcode, payload, length, APR_WAIT_MS, answer)) {
		say("sound: %s: NO ANSWER in %u ms\n", what, APR_WAIT_MS);
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->words[0] == opcode) {
		say("sound: %s: %s%s\n", what, answer->words[1] == 0 ? (wanted == 0 ? "done" : "DONE, BUT NOT THE ANSWER WANTED") :
		    "REFUSED: ", answer->words[1] == 0 ? "" : dsp_error(answer->words[1]));
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		return true;
	}
	say("sound: %s: AN ANSWER NOT EXPECTED: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
	    answer->words[0], answer->words[1]);
	return false;
}

/* ---- AFE: the port ---------------------------------------------------------------------------------- */

#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_I2S_CONFIG                 0x0001020du
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u

#define PORT                    0x1000          /* AFE_PORT_ID_PRIMARY_MI2S_RX */
#define CLOCK_PRI_MI2S_IBIT     0x100
#define CLOCK_COUPLE_NO         1               /* Q6AFE_LPASS_CLK_ATTRIBUTE_COUPLE_NO */
#define RATE                    48000
#define BITS                    16
#define BIT_CLOCK               (RATE * BITS * 2)       /* 1.536 MHz: sm8250.c's MI2S_BCLK_RATE */
#define I2S_SD1                 2               /* AFE_PORT_I2S_SD1: qcom,sd-lines <1> */
#define I2S_STEREO              1
#define I2S_WS_INTERNAL         1               /* the CPU drives the clocks: SND_SOC_DAIFMT_BP_FP */

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_i2s {
	uint32_t version;
	uint16_t bit_width, channel_mode, mono_stereo, ws_src;
	uint32_t sample_rate;
	uint16_t data_format, reserved;
} __attribute__((packed));

/* A parameter for the AFE as a whole (its clocks): in the packet, not in shared memory (q6afe_set_param). */
static bool
afe_service_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint32_t words[(16 + sizeof(struct afe_param) + 64) / 4];
	uint8_t *packet = (uint8_t *)words;
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(words, sizeof(words));
	words[0] = (uint32_t)sizeof(*p) + size;         /* payload size; address 0 and handle 0: in band */
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, 0, AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

/* A parameter of the port (q6afe_port_set_param_v2). */
static bool
afe_port_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint32_t words[(16 + sizeof(struct afe_param) + 64) / 4];
	uint8_t *packet = (uint8_t *)words;
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(words, sizeof(words));
	words[0] = PORT | (uint32_t)(sizeof(*p) + size) << 16;  /* the port, and the payload's size */
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

static bool clock_on, port_on;

static bool
afe_clock(bool enable)
{
	struct afe_clock_set clock = { 1, CLOCK_PRI_MI2S_IBIT, enable ? BIT_CLOCK : 0, CLOCK_COUPLE_NO, 0, enable };

	return afe_service_param(enable ? "the MI2S bit clock (PRI_MI2S_IBIT 0x100) at 1.536 MHz, on" :
	           "the MI2S bit clock, off", AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static bool
afe_on(void)
{
	struct afe_i2s i2s = { 1, BITS, I2S_SD1, I2S_STEREO, I2S_WS_INTERNAL, RATE, 0, 0 };
	uint16_t start[2] = { PORT, 0 };

	announce("the DSP is asked for the MI2S bit clock, the port's I2S setup and its start (AFE)");
	clock_on = afe_clock(true);
	if (!clock_on) {
		return false;
	}
	if (!afe_port_param("the port 0x1000 (PRIMARY_MI2S_RX): I2S, 16 bits, data line 1, stereo, word select ours, 48 kHz",
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, &i2s, sizeof(i2s))) {
		return false;
	}
	port_on = command("the port 0x1000, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL);
	return port_on;
}

static void
afe_off(void)
{
	uint16_t stop[2] = { PORT, 0 };

	if (port_on) {
		command("the port 0x1000, stopped", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL);
		port_on = false;
	}
	if (clock_on) {
		afe_clock(false);
		clock_on = false;
	}
}

/* ---- the DSP's view of our memory: stream 0x1801 of the SMMU (HakiOS's space_*) -------------------------------- */

static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000, 0 };
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000, 0 };              /* the second page: a word per context */
static struct block apps_smmu_context = { "apps smmu, the audio context", 0, 0x1000, 0 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define PTE_ADDRESS             0x0000fffffffff000ull
#define AUDIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as hakios.wifi.c's */

#define AUDIO_STREAM            0x1801          /* pmOS: q6asm-dais iommus <&apps_smmu 0x1801 0> */
#define AUDIO_ADDRESSES         0x10000000ull   /* our page tables: any address will do; HakiOS's */
#define AUDIO_STREAM_BITS       (1ull << 32)    /* q6asm-dai.c: | (sid & 0xf) << 32 */

#define PERIODS                 8
#define PERIOD_FRAMES           960             /* 20 ms */
#define PERIOD_BYTES            (PERIOD_FRAMES * 4)
#define BUFFER_BYTES            (PERIODS * 4096)        /* a page each */

static struct {
	bool found, on, passed;         /* passed: the boot chain lets the stream through untranslated */
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables;             /* three of 4K: the root, one of the second level, one of the third */
} space;
static vm_offset_t buffer;              /* BUFFER_BYTES of ours, the periods */
static uint64_t buffer_address;         /* as the DSP is to know it */
static bool buffer_ready;

static bool
space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;

	if (scr0 & 1u << 3) {
		say("sound: this SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * space.pages * space.page != 0x100000) {
		say("sound: the SMMU says it has 2 x %u pages of %#x bytes, not a megabyte: its contexts are not looked for\n",
		    space.pages, space.page);
		return false;
	}
	space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ AUDIO_STREAM) & ~mask) == 0) {
				say("sound: match register %u covers the audio stream already: stream %#x mask %#x, s2cr %#x\n", n,
				    stream, mask, s2cr);
				if ((s2cr >> 16 & 3) == 1) {
					space.passed = true;
					return true;
				}
				say("sound: ... and does not let it through untranslated: it is somebody's, and is left alone\n");
				return false;
			}
		} else if (space.match == ~0u) {
			space.match = n;
			space.old_smr = smr;
			space.old_s2cr = s2cr;
		}
	}
	for (space.context = 0; space.context < contexts && used[space.context]; space.context++) {
	}
	if (space.match == ~0u || space.context >= contexts) {
		say("sound: the SMMU has no match register or no context left\n");
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + space.page;
	apps_smmu_context.phys = apps_smmu.phys + (space.pages + space.context) * space.page;
	say("sound: the boot chain has set nothing up for stream %#x; free: match register %u (smr %#x, s2cr %#x) and context %u, at "
	    "%#x\n", AUDIO_STREAM, space.match, space.old_smr, space.old_s2cr, space.context, apps_smmu_context.phys);
	space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	if (space.old_sctlr & 1) {
		say("sound: CONTEXT %u IS SWITCHED ON although no stream is sent through it: it is somebody's, and is left alone\n",
		    space.context);
		return false;
	}
	space.found = true;
	return true;
}

static void
space_faults(void)
{
	uint32_t status, global;

	if (!space.on) {
		return;
	}
	status = rd(apps_smmu_context.base + SMMU_CB_FSR);
	global = rd(apps_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("sound: the SMMU has refused the DSP nothing (context %u: fsr %#x; sgfsr 0)\n", space.context, status);
		return;
	}
	say("sound: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: status "
	    "%#x, stream %#x\n", space.context, status, (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 |
	    rd(apps_smmu_context.base + SMMU_CB_FAR), rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global,
	    rd(apps_smmu.base + SMMU_SGFSYNR1));
}

/* Our periods behind the DSP's addresses from AUDIO_ADDRESSES on, through a context of ours. */
static bool
space_on(void)
{
	uint64_t *root, *second, *third, table_root;
	uint32_t smr = SMMU_SMR(space.match), s2cr = SMMU_S2CR(space.match), sees = 1u << 31 | AUDIO_STREAM;

	if (space.tables != 0) {
		bzero((void *)space.tables, 3 * 4096);
	} else if (kmem_alloc(kernel_map, &space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		space.tables = 0;
		return false;
	}
	root = (uint64_t *)space.tables;
	second = (uint64_t *)(space.tables + 4096);
	third = (uint64_t *)(space.tables + 8192);
	root[AUDIO_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[AUDIO_ADDRESSES >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned page = 0; page < BUFFER_BYTES / 4096; page++) {
		third[(AUDIO_ADDRESSES >> 12 & 0x1ff) + page] = kvtophys(buffer + 4096 * page) | AUDIO_PTE;
	}
	for (vm_offset_t line = space.tables; line < space.tables + 3 * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(space.tables) | (uint64_t)space.context << 48;
	say("sound: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) for the %u KB of the periods "
	    "at %#llx, and match register %u sends stream %#x through it\n", space.context, table_root & PTE_ADDRESS,
	    BUFFER_BYTES / 1024, AUDIO_ADDRESSES, space.match, AUDIO_STREAM);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(space.context), SMMU_CBA2R_64_BIT);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(space.context), SMMU_CBAR_STAGE_1);
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)table_root);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("sound: the SMMU does not finish forgetting old translations\n");
			return false;
		}
		sleep_ms(1);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	space.on = true;
	put(&apps_smmu, "s2cr", s2cr, space.context);
	put(&apps_smmu, "smr", smr, sees);
	if (get(&apps_smmu, "smr", smr) != sees || (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) != space.context ||
	    !(get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1) ||
	    get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0) != (uint32_t)table_root) {
		say("sound: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u\n", AUDIO_STREAM,
		    space.context);
		return false;
	}
	say("sound: THE AUDIO STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", space.context);
	return true;
}

/* As found: no lock, no sleep, no log (the panic path uses it too). With hakios_smmu_lock held otherwise. */
static void
space_off_quietly(void)
{
	if (!space.on) {
		return;
	}
	wr(apps_smmu.base + SMMU_SMR(space.match), space.old_smr);
	wr(apps_smmu.base + SMMU_S2CR(space.match), space.old_s2cr);
	wr(apps_smmu_context.base + SMMU_CB_SCTLR, space.old_sctlr);
	__asm__ volatile ("dsb sy" : : : "memory");
	space.on = false;
}

/* The periods and the DSP's view of them, once a run (kept until the DSP goes down). */
static bool
space_get(void)
{
	bool ok;

	if (buffer_ready) {
		return true;
	}
	if (buffer == 0 && kmem_alloc(kernel_map, &buffer, BUFFER_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		buffer = 0;
		say("sound: no memory for the DSP's periods\n");
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	ok = (space.found || space.passed || space_find()) && (space.passed || space.on || space_on());
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!ok) {
		return false;
	}
	buffer_address = space.passed ? (kvtophys(buffer) | AUDIO_STREAM_BITS) : (AUDIO_ADDRESSES | AUDIO_STREAM_BITS);
	for (unsigned page = 1; space.passed && page < BUFFER_BYTES / 4096; page++) {
		if (kvtophys(buffer + 4096 * page) != kvtophys(buffer) + 4096 * page) {
			say("sound: the periods are not one piece of memory, and the stream is not translated\n");
			return false;
		}
	}
	buffer_ready = true;
	return true;
}

static void
space_put(void)
{
	if (space.on) {
		space_faults();
		say("sound: the audio stream and context %u of the SMMU are put back as they were found\n", space.context);
	}
	lck_mtx_lock(&hakios_smmu_lock);
	space_off_quietly();
	lck_mtx_unlock(&hakios_smmu_lock);
	space.found = false;
	buffer_ready = false;
}

/* ---- ADM and ASM: the path and the stream --------------------------------------------------------------- */

#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_PATH_PLAYBACK                       1
#define ADM_MATRIX_AUDIO_RX                     0
#define ADM_TOPOLOGY_NONE                       0x00010312u     /* NULL_COPP_TOPOLOGY */

#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_WRITE_V3            0x00010db3u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2        0x00010d98u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_WRITE_V2                   0x00010dabu
#define ASM_DATA_EVENT_WRITE_DONE_V2            0x00010d99u
#define ASM_SESSION_EVENT_RX_UNDERFLOW          0x00010c17u
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u     /* ASM_NULL_POPP_TOPOLOGY */
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define ASM_SHMEM_4K_POOL                       3               /* ADSP_MEMORY_MAP_SHMEM8_4K_POOL */
#define SESSION                                 1
#define STREAM_PORT                             (SESSION << 8 | 1)
#define WRITE_TOKEN                             0x80000000u     /* | the period: what a write's "done" carries back */
#define PCM_CHANNEL_FL                          1
#define PCM_CHANNEL_FR                          2

static volatile bool stopping;          /* the run is ending: whatever plays stops */
static bool mapped, copp_open, stream_open;
static uint32_t map_handle, copp;
static volatile uint32_t returned;      /* the periods the DSP has said it is done with (a bit each) */
static volatile uint32_t written_done, underflows, write_errors;

/* The DSP's events: the "done" of every write, and underflows. On the link's thread: never waits. */
static bool
events(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if (service != APR_SVC_ASM) {
		return false;
	}
	if (opcode == ASM_DATA_EVENT_WRITE_DONE_V2 && (token & WRITE_TOKEN)) {
		if (length >= 16 && payload[3] != 0) {
			os_atomic_inc(&write_errors, relaxed);
		}
		os_atomic_or(&returned, 1u << (token & 31), release);
		os_atomic_inc(&written_done, release);
		return true;
	}
	if (opcode == APR_BASIC_RSP_RESULT && (token & WRITE_TOKEN)) {
		/* (a write the DSP refused outright: it answers it as a command) */
		os_atomic_inc(&write_errors, relaxed);
		os_atomic_or(&returned, 1u << (token & 31), release);
		os_atomic_inc(&written_done, release);
		return true;
	}
	if (opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		os_atomic_inc(&underflows, relaxed);
		return true;
	}
	return false;
}

static bool
stream_on(void)
{
	struct {
		uint16_t pool, regions;
		uint32_t property;
		uint32_t low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)buffer_address, (uint32_t)(buffer_address >> 32),
		                          BUFFER_BYTES };
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_PLAYBACK, PORT, 0xffff, ADM_TOPOLOGY_NONE, 2, BITS, RATE,
		                           { PCM_CHANNEL_FL, PCM_CHANNEL_FR, 0, 0, 0, 0, 0, 0 } };
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_RX, 1, SESSION, 1, { 0, 0 } };
	struct {
		uint32_t flags;
		uint16_t sink, bits;
		uint32_t topology, format;
	} __attribute__((packed)) write = { 0, 0, BITS, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
	struct {
		uint32_t size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) format = { 20, 2, BITS, RATE, 1, 0, { PCM_CHANNEL_FL, PCM_CHANNEL_FR, 0, 0, 0, 0, 0, 0 } };
	uint32_t run[3] = { 0, 0, 0 };
	struct hakios_apr_answer answer;

	announce("the DSP is given our periods (ASM), a device path to the port (ADM) and a stream routed to it (ASM)");
	if (!command("our periods, mapped for the DSP", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map),
	    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, &answer)) {
		return false;
	}
	mapped = true;
	map_handle = answer.words[0];
	say("sound: our %u KB at %#llx are the DSP's handle %#x\n", BUFFER_BYTES / 1024, buffer_address, map_handle);
	if (!command("a device path to the port: playback, no processing, 2 channels of 16 bits at 48 kHz", APR_SVC_ADM, PORT,
	    ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), ADM_CMDRSP_DEVICE_OPEN_V5, &answer)) {
		return false;
	}
	if (answer.words[0] != 0) {
		say("sound: the device path: REFUSED: %s\n", dsp_error(answer.words[0]));
		return false;
	}
	copp_open = true;
	copp = answer.words[1] & 0xffff;        /* (the upper half is reserved) */
	route.copp[0] = (uint16_t)copp;
	say("sound: the device path is open: COPP %u\n", copp);
	if (!command("the stream, opened for writing: 16-bit linear PCM, no processing", APR_SVC_ASM, STREAM_PORT,
	    ASM_STREAM_CMD_OPEN_WRITE_V3, &write, sizeof(write), 0, NULL)) {
		return false;
	}
	stream_open = true;
	return command("session 1 routed to the device path", APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route),
	           0, NULL) &&
	       command("the stream's format: 2 channels of 16 bits at 48 kHz, left and right", APR_SVC_ASM, STREAM_PORT,
	           ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &format, sizeof(format), 0, NULL) &&
	       command("the session, run", APR_SVC_ASM, STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0, NULL);
}

static void
stream_off(void)
{
	uint32_t handle = map_handle;

	if (stream_open) {
		command("the stream, closed", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0, NULL);
		stream_open = false;
	}
	if (copp_open) {
		command("the device path, closed", APR_SVC_ADM, (uint16_t)copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0, NULL);
		copp_open = false;
	}
	if (mapped) {
		command("our periods, unmapped", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &handle, sizeof(handle), 0, NULL);
		mapped = false;
	}
}

static void
period_flush(const int16_t *samples)
{
	for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));        /* the DSP reads memory, not our caches */
	}
	__asm__ volatile ("dsb sy");
}

static bool
period_write(unsigned period)
{
	uint64_t at = buffer_address + 4096 * period;
	uint32_t write[8] = { (uint32_t)at, (uint32_t)(at >> 32), map_handle, PERIOD_BYTES, period, 0, 0, 0 };

	return hakios_adsp_apr(APR_SVC_ASM, STREAM_PORT, WRITE_TOKEN | period, ASM_DATA_CMD_WRITE_V2, write, sizeof(write), 0, NULL);
}

/* ---- levels in dB, as gains --------------------------------------------------------------------------------- */

/* 10^(-n/20) in Q15, n = 0..19 */
static const uint16_t gain_of_db[20] = {
	32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627,
	10362, 9235, 8231, 7336, 6538, 5827, 5193, 4629, 4125, 3677,
};

static uint32_t
gain_q15(uint32_t db)
{
	uint32_t gain = gain_of_db[db % 20];

	for (uint32_t tens = db / 20; tens > 0 && gain != 0; tens--) {
		gain = gain * 3277 >> 15;               /* -20 dB */
	}
	return gain;
}

/* ---- the test tone ----------------------------------------------------------------------------------- */

/* 1 kHz at 48 kHz, -20 dBFS (a peak of 3277): one cycle, no DC */
static const int16_t tone[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249,
	3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638, 1254, 848, 428,
	0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249,
	-3277, -3249, -3165, -3028, -2838, -2600, -2317, -1995, -1638, -1254, -848, -428,
};
#define FADE_FRAMES             4800            /* 100 ms in, 100 ms out */

/* Frame n of a tone of `frames' frames: faded in and out, at `gain' (Q15); silence after it. */
static int16_t
tone_sample(uint32_t n, uint32_t frames, uint32_t gain)
{
	uint32_t fade = frames / 2 < FADE_FRAMES ? frames / 2 : FADE_FRAMES, from_end, envelope;
	int32_t sample;

	if (n >= frames || fade == 0) {
		return 0;
	}
	from_end = frames - 1 - n;
	envelope = n < fade ? n : from_end < fade ? from_end : fade;
	sample = (int32_t)tone[n % 48] * (int32_t)gain >> 15;
	return (int16_t)(sample * (int32_t)envelope / (int32_t)fade);
}

static uint32_t tone_at, tone_total, tone_gain;

static void
period_fill_tone(unsigned period)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);

	for (unsigned frame = 0; frame < PERIOD_FRAMES; frame++) {
		int16_t sample = tone_sample(tone_at++, tone_total, tone_gain);

		samples[2 * frame] = sample;
		samples[2 * frame + 1] = sample;
	}
	period_flush(samples);
}

/*
 * The tone, written period after period as the DSP gives them back, for `ms' (and two periods of silence after it);
 * then the DSP is waited for until it has given every period back. What came back and how fast is said.
 */
static bool
play(uint32_t ms, bool amplifiers)
{
	uint64_t began, last_look, ended;
	uint32_t sent = 0, done_before = written_done, healthy_looks = 0, end_frames;
	bool ok = true, tone_over = false;

	os_atomic_store(&returned, 0, relaxed);
	tone_at = 0;
	tone_total = ms * (RATE / 1000);
	tone_gain = amplifiers ? gain_q15(atten_db) : 32768;
	end_frames = tone_total + 2 * PERIOD_FRAMES;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill_tone(p);
	}
	began = last_look = now_us();
	for (unsigned p = 0; p < PERIODS; p++) {
		if (!period_write(p)) {
			say("sound: the write of period %u could not be sent\n", p);
			return false;
		}
		sent++;
	}
	for (;;) {
		uint32_t back = os_atomic_load(&returned, acquire);
		uint64_t now = now_us();

		if (back != 0) {
			os_atomic_and(&returned, ~back, relaxed);
			for (unsigned p = 0; p < PERIODS && !tone_over; p++) {
				if (!(back & 1u << p)) {
					continue;
				}
				period_fill_tone(p);
				if (!period_write(p)) {
					say("sound: the write of period %u could not be sent\n", p);
					ok = false;
					tone_over = true;
					break;
				}
				sent++;
			}
		}
		if (!tone_over && (tone_at >= end_frames || stopping)) {
			tone_over = true;
		}
		if (tone_over && os_atomic_load(&written_done, acquire) - done_before >= sent) {
			break;
		}
		if (now - began > ms * 1000ull + 2000000) {
			say("sound: THE DSP HAS NOT GIVEN EVERY PERIOD BACK 2 s after the end (%u of %u)\n", written_done - done_before,
			    sent);
			ok = false;
			break;
		}
		if (written_done == done_before && now - began > 1000000) {
			say("sound: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND\n");
			ok = false;
			break;
		}
		if (amplifiers && now - last_look >= 200000) {
			last_look = now;
			healthy_looks++;
			if (!dlisa_tfa_healthy()) {
				ok = false;
				tone_over = true;
			}
		}
		sleep_ms(2);
	}
	ended = now_us();
	{
		uint32_t done = written_done - done_before;
		uint64_t frames = (uint64_t)done * PERIOD_FRAMES, us = ended - began;

		say("sound: %u periods written, %u given back (%u with an error), %u underflows, in %llu ms: the DSP took %llu frames a "
		    "second%s\n", sent, done, write_errors, underflows, us / 1000, us ? frames * 1000000 / us : 0,
		    amplifiers ? "" : " (the amplifiers off: nothing to hear)");
		if (amplifiers) {
			say("sound: the amplifiers looked at %u times while it played\n", healthy_looks);
		}
	}
	return ok;
}

/* ---- sound=4: what Darwin plays ------------------------------------------------------------------------------ */

/*
 * /dev/hakios-audio's writer (the audio server's real-time thread, hakios.audio.c) puts frames into a ring and does
 * not wait; the feeder, a thread of ours, takes them from there into each period the DSP gives back (silence when
 * there is nothing), through the trim (DC blocker, gain, clip), so the stream never stops while the run lasts. The
 * watcher (the sound's thread) powers the amplifiers up when a write is not silence and down after three seconds of
 * silence, and follows Darwin's volume control.
 */
#define RING_FRAMES             32768           /* 0.68 s; a power of two, the indices run on and wrap */
#define SILENCE                 8               /* samples no larger than this are silence (as hakios.audio.c's) */
#define QUIET_BEFORE_OFF_US     3000000ull
#define GAIN_STEP               1638            /* per period (20 ms): from 0 to full in 400 ms, and back */
#define DC_POLE                 32604           /* 0.995 in Q15: about 40 Hz at 48 kHz */

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;  /* frames: written by the sink, taken by the feeder */
static volatile uint64_t last_sound_us;         /* when the latest write that was not silence came */
static volatile bool feeder_runs;
static uint64_t fed_frames, silent_frames, darwin_frames;
static volatile uint32_t darwin_peak;           /* the largest sample since the amplifiers last started */
static volatile uint32_t gain_target;           /* Q15: set by the watcher, 0 while the amplifiers are off */
static uint32_t gain_now;                       /* the feeder's, ramping to gain_target */
static int32_t dc_in[2], dc_out[2];
static volatile uint32_t tone_frames_left;      /* kern.hakios.sound_tone at sound=4: the tone in place of Darwin */
static uint32_t tone4_total, tone4_at;

/* From the writer's thread, under hakios.audio.c's lock: never waits. */
static uint32_t
darwin_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t left = frames[2 * i], right = frames[2 * i + 1];
		uint32_t l = left < 0 ? (uint32_t)-(int32_t)left : (uint32_t)left, r = right < 0 ? (uint32_t)-(int32_t)right : (uint32_t)right;

		ring[2 * ((head + i) % RING_FRAMES)] = left;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = right;
		sound |= l > SILENCE || r > SILENCE;
		if (l > darwin_peak || r > darwin_peak) {
			darwin_peak = l > r ? l : r;
		}
	}
	os_atomic_thread_fence(release);
	ring_head = head + taken;
	darwin_frames += taken;
	if (sound) {
		last_sound_us = now_us();
	}
	return taken;
}

static int16_t
clip(int64_t value)
{
	return value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
}

/* The DC blocker, the gain (ramped across the period from where it was) and the clip, in place. */
static void
trim(int16_t *samples, uint32_t frames)
{
	uint32_t from = gain_now, to = gain_target;

	to = to > from + GAIN_STEP ? from + GAIN_STEP : to + GAIN_STEP < from ? from - GAIN_STEP : to;
	for (uint32_t i = 0; i < frames; i++) {
		int64_t gain = (int64_t)from + ((int64_t)to - (int64_t)from) * (int64_t)i / (int64_t)frames;

		for (unsigned c = 0; c < 2; c++) {
			int32_t x = samples[2 * i + c];
			int32_t y = x - dc_in[c] + (int32_t)((int64_t)dc_out[c] * DC_POLE >> 15);

			dc_in[c] = x;
			dc_out[c] = y;
			samples[2 * i + c] = clip((int64_t)y * gain >> 15);
		}
	}
	gain_now = to;
}

/* A period from the ring (or the test tone), the rest of it silence, through the trim. */
static void
period_fill_darwin(unsigned period)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);
	uint32_t tail = ring_tail, available = ring_head - tail, n = available < PERIOD_FRAMES ? available : PERIOD_FRAMES;

	if (tone_frames_left != 0) {
		for (uint32_t i = 0; i < PERIOD_FRAMES; i++) {
			int16_t sample = tone_sample(tone4_at++, tone4_total, 32768);

			samples[2 * i] = sample;
			samples[2 * i + 1] = sample;
		}
		tone_frames_left = tone4_at >= tone4_total ? 0 : tone4_total - tone4_at;
		last_sound_us = now_us();
		ring_tail = ring_head;          /* (what Darwin wrote meanwhile is dropped) */
	} else {
		os_atomic_thread_fence(acquire);
		for (uint32_t i = 0; i < n; i++) {
			samples[2 * i] = ring[2 * ((tail + i) % RING_FRAMES)];
			samples[2 * i + 1] = ring[2 * ((tail + i) % RING_FRAMES) + 1];
		}
		bzero(samples + 2 * n, (PERIOD_FRAMES - n) * 4);
		os_atomic_thread_fence(release);
		ring_tail = tail + n;
		fed_frames += n;
		silent_frames += PERIOD_FRAMES - n;
	}
	trim(samples, PERIOD_FRAMES);
	period_flush(samples);
}

static void
feeder_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t sent = 0, failed = 0;

	os_atomic_store(&returned, 0, relaxed);
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill_darwin(p);
		sent += period_write(p);
	}
	while (!stopping) {
		uint32_t back = os_atomic_load(&returned, acquire);

		if (back != 0) {
			os_atomic_and(&returned, ~back, relaxed);
			for (unsigned p = 0; p < PERIODS; p++) {
				if (back & 1u << p) {
					period_fill_darwin(p);
					if (period_write(p)) {
						sent++;
					} else {
						failed++;
					}
				}
			}
		}
		sleep_ms(2);
	}
	say("sound: the feeder stops: %u periods written (%u could not be sent), %u given back (%u with an error), %u underflows; "
	    "from Darwin %llu frames came, %llu went to the DSP, %llu of silence filled in\n", sent, failed, written_done, write_errors,
	    underflows, darwin_frames, fed_frames, silent_frames);
	feeder_runs = false;
	thread_terminate(current_thread());
}

/* sound_atten and Darwin's volume (`extra', eighths of a dB under the loudest), as one gain; 0 when muted */
static uint32_t
gain_with(uint32_t extra, bool muted)
{
	uint32_t db = atten_db + (extra + 4) / 8;

	return muted ? 0 : gain_q15(db > 90 ? 90 : db);
}

/* The amplifiers on while there is sound, at the volume Darwin has set, off while it is muted. On the sound's thread. */
static void
watch(void)
{
	bool on = false, given_up = false, muted = false;
	uint64_t last_look = 0;
	uint32_t starts = 0, changes = 0, generation = 0, extra = 0, gain = gain_q15(atten_db);

	while (!stopping) {
		uint64_t now = now_us(), sounded = last_sound_us;
		uint32_t extra_now, generation_now;
		bool muted_now;

		if (hakios_audio_volume(&extra_now, &muted_now, &generation_now) && generation_now != generation) {
			generation = generation_now;
			extra = extra_now;
			muted = muted_now;
			changes++;
			gain = gain_with(extra, muted);
			say("sound: Darwin sets the volume: %s%u.%03u dB under the loudest (which is %u dB under full scale)%s\n",
			    muted ? "muted, " : "", extra / 8, extra % 8 * 125, atten_db, on && muted ? ": the amplifiers are stopped" : "");
			if (on) {
				gain_target = gain;
			}
			if (muted && on) {
				sleep_ms(500);          /* (the ramp down, and the periods queued at the DSP, played out) */
				dlisa_tfa_stop();
				on = false;
			}
		}
		if (!on && !given_up && !muted && sounded != 0 && now - sounded < 100000) {
			say_when("Darwin plays: the amplifiers are powered up");
			starts++;
			darwin_peak = 0;
			gain_now = 0;           /* (faded in from silence) */
			if (!dlisa_tfa_start()) {
				say("sound: THE AMPLIFIERS DID NOT START; Darwin's sound goes on to the DSP, unheard\n");
				given_up = true;
			} else {
				on = true;
				gain_target = gain;
				last_look = now_us();
			}
		} else if (on && now - sounded > QUIET_BEFORE_OFF_US) {
			say_when("three seconds of silence: the amplifiers are powered down");
			say("sound: what Darwin wrote while they were on peaked at %u%% of full scale (%u)\n", darwin_peak * 100 / 32768,
			    darwin_peak);
			gain_target = 0;
			dlisa_tfa_stop();
			on = false;
		} else if (on && now - last_look >= 200000) {
			last_look = now;
			if (!dlisa_tfa_healthy()) {
				gain_target = 0;
				on = false;
				given_up = true;
			}
		}
		sleep_ms(5);
	}
	gain_target = 0;
	say("sound: the watcher stops; the amplifiers were started %u times, Darwin set the volume %u times%s\n", starts, changes,
	    given_up ? ", and the amplifiers were then given up on" : "");
}

/* ---- the whole ---------------------------------------------------------------------------------------- */

static volatile bool running, taken_down, busy, serving;

/* Everything a level started, stopped: amplifiers first while the clock still runs. With sound_lock held. */
static void
stop_all(void)
{
	dlisa_tfa_stop();
	stream_off();
	afe_off();
	pins_off();
}

/* Levels 1 to 3: one run of the port (and the stream, the tone, the amplifiers). With sound_lock held. */
static bool
run_test(uint32_t ms)
{
	bool ok = false;
	uint32_t clock_changes, select_changes;

	busy = true;
	if (level == 1) {
		say("sound: level 1: the speakers' MI2S port for %u s\n", ms / 1000);
	} else {
		say("sound: level %u: the port and a stream with a test tone of %u ms, %s\n", level, ms, level == 2 ?
		    "the amplifiers off (1 kHz, -20 dBFS)" : "and the amplifiers");
	}
	state(level == 1 ? "the port" : level == 2 ? "the port and a stream, unheard" : "a test tone, heard");
	if (level >= 2 && !space_get()) {
		goto out;
	}
	announce("the MI2S pins (GPIO 97 to 100) to the interface, as pmOS's active state has them");
	pins_on();
	if (!afe_on()) {
		goto out;
	}
	sleep_ms(20);
	clock_changes = pin_activity(97, "the bit clock (gpio97)");
	select_changes = pin_activity(100, "the word select (gpio100)");
	say("sound: %s\n", clock_changes > 100 && select_changes > 4 ? "THE MI2S BIT CLOCK AND WORD SELECT RUN at the pins" :
	    "THE PINS DO NOT SHOW A RUNNING MI2S CLOCK");
	if (level == 1) {
		sleep_ms(ms);
		ok = true;
		goto out;
	}
	if (!stream_on()) {
		goto out;
	}
	if (level >= 3) {
		/* the clock runs and the stream plays silence: the amplifiers can lock to it */
		announce("the amplifiers: set up as pmOS's tfa9872.c does, then powered up and unmuted while silence plays");
		if (!dlisa_tfa_prepare() || !dlisa_tfa_start()) {
			goto out;
		}
		say("sound: THE TONE PLAYS NOW for %u ms: 1 kHz at -%u dBFS, both channels, faded in and out\n", ms, 20 + atten_db);
	}
	say_when("the tone begins");
	ok = play(ms, level >= 3);
	say_when("the tone has ended");
	space_faults();
out:
	stop_all();
	tones_played += ok && level >= 2;
	say("sound: %s; last: %s\n", ok ? "LEVEL DONE" : "STOPPED SHORT", last_write);
	state(ok ? "done" : "STOPPED SHORT (see the log)");
	busy = false;
	return ok;
}

static void
take_down(void)
{
	stopping = true;
	if (serving) {
		gain_target = 0;
		hakios_audio_set_sink("nothing", NULL);
	}
	for (unsigned ms = 0; ms < 3000 && (feeder_runs || serving); ms++) {
		sleep_ms(1);
	}
	lck_mtx_lock(&sound_lock);
	if (running && !taken_down) {
		taken_down = true;
		stop_all();
		space_put();
		hakios_adsp_apr_events(NULL);
		say("sound: everything of the sound's is stopped and put back\n");
		state("taken down (the DSP is going down)");
	}
	lck_mtx_unlock(&sound_lock);
}

/* sound=4 */
static void
serve(void)
{
	thread_t feeder;

	say("sound: level 4: what Darwin plays, through the port, a stream and the amplifiers, until the run ends\n");
	state("serving Darwin: starting");
	if (!space_get()) {
		goto out;
	}
	announce("the MI2S pins (GPIO 97 to 100) to the interface, as pmOS's active state has them");
	pins_on();
	if (!afe_on() || !stream_on()) {
		goto out;
	}
	announce("the amplifiers, set up as pmOS's tfa9872.c does, powered down until Darwin plays");
	if (!dlisa_tfa_prepare()) {
		goto out;
	}
	feeder_runs = true;
	if (kernel_thread_start(feeder_thread, NULL, &feeder) != KERN_SUCCESS) {
		feeder_runs = false;
		say("sound: no thread for the feeder\n");
		goto out;
	}
	thread_deallocate(feeder);
	serving = true;
	hakios_audio_set_sink("the speakers (the audio DSP, dlisa.sound.c)", darwin_sink);
	say("sound: WHAT DARWIN PLAYS GOES TO THE SPEAKERS from here on, %u dB under full scale at the loudest, until the run ends\n",
	    atten_db);
	state("serving Darwin");
	lck_mtx_unlock(&sound_lock);
	watch();
	lck_mtx_lock(&sound_lock);
	serving = false;
	return;
out:
	stop_all();
	say("sound: STOPPED SHORT; last: %s\n", last_write);
	state("STOPPED SHORT (see the log)");
}

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	lck_mtx_lock(&sound_lock);
	if (!taken_down) {
		/* (level 1 leaves the amplifiers' bus alone; from level 2 on they are looked at: I2C reads only) */
		bool looked = level >= 2 ? dlisa_tfa_look() : true;

		if (level >= 3 && !looked) {
			say("sound: the amplifiers are not both as expected (sound_amps=, sound_amp_reset=1): nothing is started\n");
			state("STOPPED: the amplifiers (see the log)");
		} else if (level >= 2) {
			hakios_adsp_apr_events(events);
			level == 4 ? serve() : (void)run_test(seconds * 1000);
		} else {
			(void)run_test(seconds * 1000);
		}
	}
	lck_mtx_unlock(&sound_lock);
	thread_terminate(current_thread());
}

void
hakios_sound_dsp_up(void)
{
	thread_t thread;
	uint32_t amps = DLISA_TFA_TOP | DLISA_TFA_BOTTOM, reset = 0, dcdc = 1;

	if (!PE_parse_boot_argn("sound", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 4 ? 4 : level;
	PE_parse_boot_argn("sound_secs", &seconds, sizeof(seconds));
	PE_parse_boot_argn("sound_atten", &atten_db, sizeof(atten_db));
	PE_parse_boot_argn("sound_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("sound_amps", &amps, sizeof(amps));
	PE_parse_boot_argn("sound_amp_reset", &reset, sizeof(reset));
	PE_parse_boot_argn("sound_dcdc", &dcdc, sizeof(dcdc));
	seconds = seconds == 0 ? 1 : seconds > 10 ? 10 : seconds;
	if (level >= 2 && seconds > TONE_MAX_MS / 1000) {
		seconds = TONE_MAX_MS / 1000;           /* the test tone: three seconds at most */
	}
	atten_db = atten_db < ATTEN_MIN_DB ? ATTEN_MIN_DB : atten_db > ATTEN_MAX_DB ? ATTEN_MAX_DB : atten_db;
	pause_ms = pause_ms > 5000 ? 5000 : pause_ms;
	dlisa_tfa_configure(amps, reset != 0, dcdc != 0);
	say("sound: level %u (sound_secs %u, sound_atten %u dB, sound_amps %u%s%s); the DSP's audio services are up\n", level, seconds,
	    atten_db, amps & 3, reset ? ", sound_amp_reset" : "", dcdc ? "" : ", sound_dcdc=0");
	state("starting");
	running = true;
	if (kernel_thread_start(sound_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		say("sound: no thread\n");
		state("no thread");
		return;
	}
	thread_deallocate(thread);
}

void
hakios_sound_dsp_down(void)
{
	if (running) {
		take_down();
	}
}

void
hakios_sound_quiesce(void)
{
	dlisa_tfa_quiesce();
	if (space.on && apps_smmu.base != 0 && apps_smmu_context.base != 0) {
		space_off_quietly();
	}
}

/* ---- kern.hakios.sound_status, kern.hakios.sound_tone ------------------------------------------------------------- */

static void
tone_thread(void *parameter, __unused wait_result_t result)
{
	lck_mtx_lock(&sound_lock);
	if (!taken_down && !stopping) {
		(void)run_test((uint32_t)(uintptr_t)parameter);
	}
	busy = false;
	lck_mtx_unlock(&sound_lock);
	thread_terminate(current_thread());
}

static int
tone_now(uint32_t ms)
{
	thread_t thread;

	ms = ms < TONE_MIN_MS ? TONE_MIN_MS : ms > TONE_MAX_MS ? TONE_MAX_MS : ms;
	if (!running || taken_down || stopping || level < 3) {
		return ENODEV;
	}
	if (level == 4) {
		if (!serving || tone_frames_left != 0) {
			return EBUSY;
		}
		say("sound: kern.hakios.sound_tone: %u ms of the test tone, as if Darwin played it\n", ms);
		tone4_at = 0;
		tone4_total = ms * (RATE / 1000);
		tone_frames_left = tone4_total;
		tones_played++;
		return 0;
	}
	if (busy) {
		return EBUSY;
	}
	busy = true;
	say("sound: kern.hakios.sound_tone: the test tone again, %u ms\n", ms);
	if (kernel_thread_start(tone_thread, (void *)(uintptr_t)ms, &thread) != KERN_SUCCESS) {
		busy = false;
		return ENOMEM;
	}
	thread_deallocate(thread);
	return 0;
}

SYSCTL_DECL(_kern_hakios);

static int
sysctl_sound_tone SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = (int)tones_played, error = SYSCTL_OUT(req, &value, sizeof(value));

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	error = SYSCTL_IN(req, &value, sizeof(value));
	return error != 0 ? error : value <= 0 ? EINVAL : tone_now((uint32_t)value);
}

static int
sysctl_sound_status SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[400];

	snprintf(text, sizeof(text), "level %u: %s; amplifiers: %s; periods given back %u (errors %u), underflows %u; tones %u; "
	    "from Darwin %llu frames; last: %s", level, state_text, dlisa_tfa_state(), written_done, write_errors, underflows,
	    tones_played, darwin_frames, last_write);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, sound_tone, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_sound_tone, "I",
    "write MS: the speakers' test tone now (sound=3: the whole path again; sound=4: in place of Darwin); reads the tones played");
SYSCTL_PROC(_kern_hakios, OID_AUTO, sound_status, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_sound_status,
    "A", "the speakers' path (dlisa.sound.c): level, state, the amplifiers, the DSP's counts");
