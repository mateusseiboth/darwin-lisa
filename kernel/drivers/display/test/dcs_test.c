/*
 * Host test of dlisa_dcs.h: the MSM DSI command-DMA words of the panel's DCS packets, the read-back parse and the
 * level limits. sh kernel/drivers/display/test/run-tests.sh
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "dlisa_dcs.h"

static int fails;

static void
expect(bool ok, const char * what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	fails += !ok;
}

/* 0x51 with the level big-endian: a DCS long write (0x39), word count 3, last packet + long; payload padded with 0xff */
static void
testBrightnessPacket(void)
{
	uint32_t words[4] = { 0 };
	unsigned n = dcsBrightnessPacket(words, 4, 0x7ff);

	expect(n == 2, "0x51: two words");
	expect(words[0] == 0xc0390003u, "0x51: header = wc 3, type 0x39, last|long");
	expect(words[1] == 0xffff0751u, "0x51 0x07 0xff + 0xff pad (bytes 51 07 ff ff)");
	n = dcsBrightnessPacket(words, 4, 0x133);
	expect(n == 2 && words[1] == 0xff330151u, "0x51 0x01 0x33 (the panel's init level)");
	expect(dcsBrightnessPacket(words, 1, 0x100) == 0, "0x51: no room, nothing");
}

/* a short write with one parameter (0x15): 53 28 = dimming on */
static void
testShortWrite(void)
{
	uint32_t words[2] = { 0 };

	expect(dcsShortWrite1Packet(words, 2, 0x53, 0x28) == 1, "0x53: one word");
	expect(words[0] == 0x80152853u, "0x53 0x28: bytes 53 28 15 80");
}

/* set maximum return packet size (0x37) is a SHORT packet with the size in its two data bytes */
static void
testMaxReturn(void)
{
	uint32_t words[2] = { 0 };

	expect(dcsMaxReturnPacket(words, 2, 2) == 1, "0x37: one word");
	expect(words[0] == 0x80370002u, "0x37 size 2: bytes 02 00 37 80");
}

/* a DCS read (0x06) of 0x52: the command byte as it is (not 0x72), the read flag (BTA) in the last byte */
static void
testReadPacket(void)
{
	uint32_t words[2] = { 0 };

	expect(dcsReadPacket(words, 2, 0x52) == 1, "0x06: one word");
	expect(words[0] == 0xa0060052u, "read 0x52: bytes 52 00 06 a0");
}

static void
testParse(void)
{
	uint32_t rdbk[4] = { 0 };
	uint8_t out[2] = { 0 };

	/* short read response, 2 bytes (0x22): RDBK0 = type, d0, d1, ecc */
	rdbk[0] = 0x2207ff1au;
	expect(dcsParseRead(rdbk, out, 2) == 2 && out[0] == 0x07 && out[1] == 0xff, "short 2-byte response -> 07 ff");
	/* short read response, 1 byte (0x21) asked for 2: one byte */
	rdbk[0] = 0x21330000u;
	expect(dcsParseRead(rdbk, out, 2) == 1 && out[0] == 0x33, "short 1-byte response -> 33");
	/* long read response (0x1c), word count 2: RDBK1 = header, RDBK0 = d0 d1 crc crc */
	rdbk[1] = 0x1c020011u;
	rdbk[0] = 0x01338a5cu;
	expect(dcsParseRead(rdbk, out, 2) == 2 && out[0] == 0x01 && out[1] == 0x33, "long response wc 2 -> 01 33");
	/* acknowledge and error report (0x02) */
	rdbk[1] = 0;
	rdbk[0] = 0x02000100u;
	expect(dcsParseRead(rdbk, out, 2) == DCS_READ_ERROR_REPORT, "0x02: the panel reported an error");
	rdbk[0] = 0;
	expect(dcsParseRead(rdbk, out, 2) == DCS_READ_NOTHING, "all zero: nothing came back");
}

/* the level never goes past the normal range (0x7ff): above is the panel's high brightness mode */
static void
testLimits(void)
{
	expect(dcsClampLevel(5000, 1, 4095) == 0x7ff, "4095 asked as maximum: 0x7ff all the same");
	expect(dcsClampLevel(2047, 1, 2047) == 2047, "2047 kept");
	expect(dcsClampLevel(-3, 1, 2047) == 1, "below the minimum: the minimum");
	expect(dcsClampLevel(500, 10, 400) == 400, "a lower maximum (bright_max) holds");
	expect(dcsClampLevel(500, 900, 400) == 400, "minimum above maximum: the maximum wins");
}

/* CoreBrightness's brightness-nits (16.16) to a level: linear between the published minimum and maximum nits */
static void
testNits(void)
{
	const int32_t two = 2 << 16, five_hundred = 500 << 16;

	expect(dcsLevelForNits(500 << 16, two, five_hundred) == 2047, "500 nits of 2..500: 2047");
	expect(dcsLevelForNits(2 << 16, two, five_hundred) == 1, "2 nits: level 1");
	expect(dcsLevelForNits(0, two, five_hundred) == 1, "0 nits: level 1, never black");
	expect(dcsLevelForNits(-5 * 65536, two, five_hundred) == 1, "negative: level 1");
	expect(dcsLevelForNits(674 << 16, two, five_hundred) == 2047, "above the maximum: 2047, never HBM");
	expect(dcsLevelForNits(251 << 16, two, five_hundred) == 1024, "the middle of the nits: the middle level");
	expect(dcsLevelForNits(100 << 16, two, two) == 2047, "a range of nothing: the maximum, no division by zero");
}

/* the nits a frame-synchronous swap carries are an IEEE double; the kernel has no floating point, so its bits are read */
static uint64_t
bitsOf(double value)
{
	uint64_t bits;

	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static void
testNitsFromDouble(void)
{
	expect(dcsNits16FromDoubleBits(bitsOf(155.0)) == 155 << 16, "155.0 -> 155 << 16");
	expect(dcsNits16FromDoubleBits(bitsOf(2.5)) == 163840, "2.5 -> 2.5 * 65536");
	expect(dcsNits16FromDoubleBits(bitsOf(674.03)) == (int32_t) (674.03 * 65536), "674.03, truncated");
	expect(dcsNits16FromDoubleBits(bitsOf(0.0)) == 0, "0.0 -> 0");
	expect(dcsNits16FromDoubleBits(bitsOf(0.00001)) == 0, "under a 65536th -> 0");
	expect(dcsNits16FromDoubleBits(bitsOf(-3.0)) == 0, "negative -> 0");
	expect(dcsNits16FromDoubleBits(bitsOf(1e12)) == INT32_MAX, "huge -> the largest");
	expect(dcsNits16FromDoubleBits(0x7ff8000000000000ULL) == 0, "NaN -> 0");
	expect(dcsNits16FromDoubleBits(0x7ff0000000000000ULL) == INT32_MAX, "infinity -> the largest");
}

int
main(void)
{
	testBrightnessPacket();
	testShortWrite();
	testMaxReturn();
	testReadPacket();
	testParse();
	testLimits();
	testNits();
	testNitsFromDouble();
	printf("%d failures\n", fails);
	return fails != 0;
}
