/*
 * darwin-lisa: the panel's DCS packets as the MSM DSI host's command DMA reads them, the parse of a DCS read's answer and
 * the limits of the brightness level. Pure C (no kernel headers), so the host test (test/dcs_test.c) runs the very code
 * the kernel does (MiniPanelBrightness.cpp).
 *
 * The DMA format is Linux's (drivers/gpu/drm/msm/dsi/dsi_host.c, dsi_cmd_dma_add): per packet a 4-byte header
 * { data0, data1, data type (virtual channel 0), flags } with the flags 0x80 (last packet), 0x40 (long packet) and
 * 0x20 (a read: the host turns the bus around for the answer). The ECC is the hardware's. A long packet's word count is
 * data0/data1 and its payload follows, padded with 0xff to a whole word.
 *
 * The panel (CSOT VTDR6130, Xiaomi's k9d 36 02 0a, bl_ctrl_dcs): 0x51 takes the level big-endian in two bytes, 12 bits.
 * 1..0x7ff is the normal range; 0x800..0xfff is its high brightness mode (Xiaomi's hbm-on sends 51 0f ff), which this
 * port never asks for: dcsClampLevel() stops at 0x7ff whatever the caller's maximum.
 */
#ifndef DLISA_DCS_H
#define DLISA_DCS_H

#include <stdint.h>

#define DCS_DT_SHORT_WRITE_1    0x15            /* DCS short write, one parameter */
#define DCS_DT_READ             0x06            /* DCS read, no parameter */
#define DCS_DT_LONG_WRITE       0x39            /* DCS long write */
#define DSI_DT_MAX_RETURN       0x37            /* set maximum return packet size (a short packet) */

#define DCS_FLAG_LAST           0x80u
#define DCS_FLAG_LONG           0x40u
#define DCS_FLAG_READ           0x20u

#define DCS_SET_BRIGHTNESS      0x51
#define DCS_GET_BRIGHTNESS      0x52
#define DCS_WRITE_CONTROL       0x53

#define DCS_LEVEL_NORMAL_MAX    0x7ff           /* above: high brightness mode, never sent */

#define DCS_READ_NOTHING        (-1)
#define DCS_READ_ERROR_REPORT   (-2)

static inline uint32_t
dcsWord(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
	return (uint32_t) b0 | (uint32_t) b1 << 8 | (uint32_t) b2 << 16 | (uint32_t) b3 << 24;
}

/* a short packet: one word */
static inline unsigned
dcsShortPacket(uint32_t * words, unsigned room, uint8_t type, uint8_t data0, uint8_t data1, uint8_t flags)
{
	if (room < 1) {
		return 0;
	}
	words[0] = dcsWord(data0, data1, type, (uint8_t) (DCS_FLAG_LAST | flags));
	return 1;
}

static inline unsigned
dcsShortWrite1Packet(uint32_t * words, unsigned room, uint8_t command, uint8_t parameter)
{
	return dcsShortPacket(words, room, DCS_DT_SHORT_WRITE_1, command, parameter, 0);
}

static inline unsigned
dcsMaxReturnPacket(uint32_t * words, unsigned room, uint16_t size)
{
	return dcsShortPacket(words, room, DSI_DT_MAX_RETURN, (uint8_t) size, (uint8_t) (size >> 8), 0);
}

static inline unsigned
dcsReadPacket(uint32_t * words, unsigned room, uint8_t command)
{
	return dcsShortPacket(words, room, DCS_DT_READ, command, 0, DCS_FLAG_READ);
}

/* 0x51, the level's high byte first: a long write of three bytes, two words */
static inline unsigned
dcsBrightnessPacket(uint32_t * words, unsigned room, uint16_t level)
{
	if (room < 2) {
		return 0;
	}
	words[0] = dcsWord(3, 0, DCS_DT_LONG_WRITE, (uint8_t) (DCS_FLAG_LAST | DCS_FLAG_LONG));
	words[1] = dcsWord(DCS_SET_BRIGHTNESS, (uint8_t) (level >> 8), (uint8_t) level, 0xff);
	return 2;
}

static inline int
isDcsShortResponse(uint8_t type)
{
	return type == 0x21 || type == 0x22 || type == 0x11 || type == 0x12;
}

static inline int
isDcsLongResponse(uint8_t type)
{
	return type == 0x1c || type == 0x1a;
}

/*
 * The answer of a DCS read, from the host's four read-back registers: RDBK0 holds the last four bytes that came in,
 * the first of them in its top byte (Linux's dsi_cmd_dma_rx turns each with ntohl). A short response is one word
 * (type, data0, data1, ECC); a long one is a header word (type, word count, ECC) then the data and a 2-byte CRC.
 * Returns the bytes put in `out` (at most `room`), DCS_READ_ERROR_REPORT for an acknowledge-and-error report, or
 * DCS_READ_NOTHING when no answer is recognised.
 */
static inline int
dcsParseRead(const uint32_t rdbk[4], uint8_t * out, unsigned room)
{
	uint8_t type = (uint8_t) (rdbk[0] >> 24);
	unsigned count, i;

	if (isDcsShortResponse(type)) {
		count = (type & 0x0f) == 1 ? 1 : 2;
		count = count < room ? count : room;
		for (i = 0; i < count; i++) {
			out[i] = (uint8_t) (rdbk[0] >> (16 - 8 * i));
		}
		return (int) count;
	}
	if (type == 0x02) {
		return DCS_READ_ERROR_REPORT;
	}
	if (isDcsLongResponse((uint8_t) (rdbk[1] >> 24))) {
		count = ((rdbk[1] >> 16) & 0xff) | ((rdbk[1] >> 8) & 0xff) << 8;
		if (count == 0 || count > 2) {
			return DCS_READ_NOTHING;        /* longer answers do not fit this parse (two bytes are enough for 0x52) */
		}
		count = count < room ? count : room;
		for (i = 0; i < count; i++) {
			out[i] = (uint8_t) (rdbk[0] >> (24 - 8 * i));
		}
		return (int) count;
	}
	return DCS_READ_NOTHING;
}

/* the level actually sent: within [minimum, maximum], and never above the normal range */
static inline uint16_t
dcsClampLevel(int level, int minimum, int maximum)
{
	int top = maximum < DCS_LEVEL_NORMAL_MAX ? maximum : DCS_LEVEL_NORMAL_MAX;
	int bottom = minimum < top ? minimum : top;

	if (level < bottom) {
		return (uint16_t) (bottom < 0 ? 0 : bottom);
	}
	return (uint16_t) (level > top ? top : level);
}

/*
 * CoreBrightness's brightness-nits (signed 16.16) to a level. An AMOLED's level is about linear in light, so the published
 * range [minimum, maximum] nits maps linearly onto 1..0x7ff. Nothing at or under the minimum goes darker than level 1.
 */
static inline uint16_t
dcsLevelForNits(int32_t nits, int32_t minimum, int32_t maximum)
{
	int64_t span = (int64_t) maximum - minimum;

	if (span <= 0 || nits >= maximum) {
		return DCS_LEVEL_NORMAL_MAX;
	}
	if (nits <= minimum) {
		return 1;
	}
	return (uint16_t) (1 + ((int64_t) (nits - minimum) * (DCS_LEVEL_NORMAL_MAX - 1) + span / 2) / span);
}

/*
 * An IEEE 754 double (the nits of a frame-synchronous swap, IOMobileFramebufferSwapSetBrightness) as signed 16.16, from its
 * bits: the kernel is built without floating point. Negative, NaN and anything under 1/65536 give 0; too large, INT32_MAX.
 */
static inline int32_t
dcsNits16FromDoubleBits(uint64_t bits)
{
	int exponent = (int) ((bits >> 52) & 0x7ff);
	uint64_t mantissa = (bits & 0xfffffffffffffULL) | (1ULL << 52);
	int shift;

	if (exponent == 0x7ff) {
		return (bits & 0xfffffffffffffULL) != 0 ? 0 : (bits >> 63 ? 0 : INT32_MAX);
	}
	if ((bits >> 63) != 0 || exponent == 0) {
		return 0;
	}
	/* value = mantissa * 2^(exponent - 1075); times 65536 is a shift by (exponent - 1075 + 16) */
	shift = exponent - 1075 + 16;
	if (shift >= 0) {
		return shift > 10 ? INT32_MAX : (mantissa << shift) > INT32_MAX ? INT32_MAX : (int32_t) (mantissa << shift);
	}
	return shift <= -64 ? 0 : (int32_t) (mantissa >> -shift);
}

#endif /* DLISA_DCS_H */
