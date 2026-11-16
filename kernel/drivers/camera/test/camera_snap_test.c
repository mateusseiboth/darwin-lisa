/*
 * Host test of camera_snap.h: what the kernel says in its log about a RAW10 frame (a thumbnail, the Bayer sites'
 * levels, a checksum) and the exposure step it takes between frames. Built and run by test/run-tests.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../camera_snap.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* MIPI CSI-2 RAW10: four pixels in five bytes, the high eight bits of each, then their low two bits together */
static void
pack(uint8_t *line, const uint16_t *pixels, uint32_t width)
{
	for (uint32_t i = 0; i < width; i += 4) {
		uint8_t low = 0;

		for (uint32_t k = 0; k < 4; k++) {
			line[i / 4 * 5 + k] = (uint8_t)(pixels[i + k] >> 2);
			low |= (uint8_t)((pixels[i + k] & 3) << (2 * k));
		}
		line[i / 4 * 5 + 4] = low;
	}
}

/* a GRBG frame: green 512, red 256, blue 128 everywhere, one red site 1023 (the brightest) */
static uint8_t *
frame_grbg(uint32_t width, uint32_t height, uint32_t stride)
{
	uint8_t *frame = calloc(height, stride);
	uint16_t *line = calloc(width, sizeof(*line));

	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			bool even_row = (y & 1) == 0, even_col = (x & 1) == 0;

			line[x] = even_row == even_col ? 512 : even_row ? 256 : 128;
		}
		if (y == 4) {
			line[9] = 1023;
		}
		pack(frame + (size_t)y * stride, line, width);
	}
	free(line);
	return frame;
}

static void
test_raw10(void)
{
	uint16_t pixels[8] = { 0, 1, 2, 3, 1023, 512, 511, 4 };
	uint8_t line[16] = { 0 };

	pack(line, pixels, 8);
	for (uint32_t i = 0; i < 8; i++) {
		CHECK(snap_raw10(line, 0, i, 0) == pixels[i]);
	}
	CHECK(SNAP_RAW10_STRIDE(2592) == 3248);         /* 3240 rounded up to 16, as the write master lays a line out */
}

static void
test_stats(void)
{
	uint32_t width = 64, height = 48, stride = SNAP_RAW10_STRIDE(64);
	uint8_t *frame = frame_grbg(width, height, stride);
	struct snap_stats s;

	snap_stats(frame, width, height, stride, 1, &s);
	CHECK(s.samples == 32 * 24);
	CHECK(s.mean[0] == 512 && s.mean[3] == 512);    /* the greens of GRBG */
	CHECK(s.mean[1] == 256 || s.mean[1] == 257);    /* red, one site 1023 among 768 */
	CHECK(s.mean[2] == 128);
	CHECK(s.max == 1023 && s.min == 128);
	CHECK(s.saturated_permille == 0);               /* one site in 3072 is under one in a thousand */
	{
		uint32_t before = s.checksum;

		frame[stride * 10 + 3] ^= 0x40;
		snap_stats(frame, width, height, stride, 1, &s);
		CHECK(s.checksum != before);
	}
	free(frame);
}

static void
test_rgb_and_row(void)
{
	uint32_t width = 64, height = 48, stride = SNAP_RAW10_STRIDE(64);
	uint8_t *frame = frame_grbg(width, height, stride), rgb[3];
	char row[16 * 6 + 1];

	snap_cell_rgb(frame, stride, 0, 0, SNAP_BAYER_GRBG, rgb);
	CHECK(rgb[0] == 64 && rgb[1] == 128 && rgb[2] == 32);
	snap_cell_rgb(frame, stride, 0, 0, SNAP_BAYER_RGGB, rgb);       /* the same cell read as RGGB */
	CHECK(rgb[0] == 128 && rgb[1] == 48 && rgb[2] == 128);
	CHECK(snap_row_hex(frame, width, height, stride, SNAP_BAYER_GRBG, 16, 12, 0, row, sizeof(row)) == 16 * 6);
	CHECK(strncmp(row, "408020408020", 12) == 0);
	CHECK(strlen(row) == 96);
	CHECK(snap_row_hex(frame, width, height, stride, SNAP_BAYER_GRBG, 16, 12, 0, row, 10) == 0);  /* too small: nothing */
	free(frame);
}

static void
test_exposure(void)
{
	struct snap_exposure e = { .lines = 2028, .gain = 1024, .frame_lines = 2030, .margin = 2, .unity = 32, .gain_max = 1024 };

	/* Linux's defaults (the longest exposure, the most gain) and a frame near saturation: the gain comes down */
	snap_exposure_step(&e, 1000);
	CHECK(e.gain == 262 && e.lines == 2028);
	/* far too dark: up, gain last */
	e = (struct snap_exposure){ .lines = 100, .gain = 32, .frame_lines = 2030, .margin = 2, .unity = 32, .gain_max = 1024 };
	snap_exposure_step(&e, 16);
	CHECK(e.lines == 800 && e.gain == 32);
	/* the frame's lines are the limit, then gain makes up the rest */
	e = (struct snap_exposure){ .lines = 1500, .gain = 32, .frame_lines = 2030, .margin = 2, .unity = 32, .gain_max = 1024 };
	snap_exposure_step(&e, 64);
	CHECK(e.lines == 2028 && e.gain == 94);
	/* on target: nothing moves */
	e = (struct snap_exposure){ .lines = 1000, .gain = 64, .frame_lines = 2030, .margin = 2, .unity = 32, .gain_max = 1024 };
	CHECK(!snap_exposure_step(&e, SNAP_EXPOSURE_TARGET));
	CHECK(e.lines == 1000 && e.gain == 64);
	/* a black frame does not divide by zero */
	snap_exposure_step(&e, 0);
	CHECK(e.lines > 1000 || e.gain > 64);
}

static void
test_hex_and_crop(void)
{
	uint8_t bytes[3] = { 0x00, 0xa5, 0xff };
	char out[7];

	CHECK(snap_hex(bytes, 3, out, sizeof(out)) == 6 && strcmp(out, "00a5ff") == 0);
	CHECK(snap_hex(bytes, 3, out, 6) == 0);         /* no room for the NUL: nothing */
	/* the centre crop of a 2592x1944 frame, 256 x 192: whole RAW10 groups, even rows and columns (the Bayer phase kept) */
	{
		uint32_t x, y, w = 256, h = 192;

		snap_crop(2592, 1944, &w, &h, &x, &y);
		CHECK(w == 256 && h == 192 && x == 1168 && y == 876 && x % 4 == 0 && y % 2 == 0);
		w = 5000;
		h = 7;
		snap_crop(64, 48, &w, &h, &x, &y);
		CHECK(w == 64 && h == 6 && x == 0 && y == 20);
	}
}

int
main(void)
{
	test_hex_and_crop();
	test_raw10();
	test_stats();
	test_rgb_and_row();
	test_exposure();
	printf("camera_snap_test: %d failures\n", failures);
	return failures != 0;
}
