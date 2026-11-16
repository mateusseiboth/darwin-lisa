/*
 * darwin-lisa: what the camera driver (hakios.camera.c) says about a frame it captured, and the exposure step it takes
 * between frames. Pure functions over a RAW10 frame in memory, no kernel calls, so that test/camera_snap_test.c runs
 * them on the host (copied next to the driver as bsd/dev/hakios.camera_snap.h by camera-wire.py).
 *
 *   snap_stats()          the mean of each of the four Bayer sites, the lowest and highest level, how many sites are
 *                         saturated (per thousand) and a checksum: proof that light reached the sensor and that two
 *                         frames differ
 *   snap_row_hex()        one row of an RGB thumbnail as hex: the log's "camsnap" lines, which camsnap.py turns back
 *                         into a picture
 *   snap_exposure_step()  one step of a plain auto-exposure: the exposure (lines x gain) scaled towards a mean green of
 *                         SNAP_EXPOSURE_TARGET, at most 8x either way, the shutter first and the gain for the rest
 *
 * A frame is laid out as the IFE's write master writes it: MIPI CSI-2 RAW10 (four pixels in five bytes, their high
 * eight bits, then the four low pairs in one byte), each line SNAP_RAW10_STRIDE(width) bytes.
 */
#ifndef DLISA_CAMERA_SNAP_H
#define DLISA_CAMERA_SNAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAP_RAW10_STRIDE(width)        ((((uint32_t)(width)) * 10 / 8 + 15) & ~15u)
#define SNAP_SATURATED                  1020u
#define SNAP_EXPOSURE_TARGET            256u    /* a quarter of the 10-bit range: a room comes out neither dark nor clipped */
#define SNAP_EXPOSURE_STEP_MAX          8u

/* the Bayer orders, as camraw.py and the frames' header number them */
enum { SNAP_BAYER_RGGB = 0, SNAP_BAYER_GRBG = 1, SNAP_BAYER_GBRG = 2, SNAP_BAYER_BGGR = 3 };

struct snap_stats {
	uint32_t mean[4];                       /* the sites of a 2x2 cell in raster order: (0,0), (1,0), (0,1), (1,1) */
	uint32_t min, max;
	uint32_t saturated_permille;
	uint32_t checksum;                      /* FNV-1a over the sampled levels */
	uint32_t samples;                       /* cells */
};

struct snap_exposure {
	uint32_t lines, gain;                   /* what the sensor has now: coarse integration lines, analog gain code */
	uint32_t frame_lines, margin;           /* the frame length, and how far below it the exposure must stay */
	uint32_t unity, gain_max;               /* the gain code of 1x, and the most it is given */
};

static inline uint32_t
snap_raw10(const uint8_t *frame, uint32_t stride, uint32_t x, uint32_t y)
{
	const uint8_t *group = frame + (size_t)y * stride + (size_t)(x / 4) * 5;

	return (uint32_t)group[x % 4] << 2 | ((uint32_t)group[4] >> (2 * (x % 4)) & 3u);
}

static inline void
snap_stats(const uint8_t *frame, uint32_t width, uint32_t height, uint32_t stride, uint32_t step, struct snap_stats *s)
{
	uint64_t sum[4] = { 0, 0, 0, 0 };
	uint32_t saturated = 0, hash = 2166136261u;

	step = step == 0 ? 1 : step;
	s->min = 1023;
	s->max = 0;
	s->samples = 0;
	for (uint32_t cy = 0; cy < height / 2; cy += step) {
		for (uint32_t cx = 0; cx < width / 2; cx += step) {
			for (uint32_t site = 0; site < 4; site++) {
				uint32_t v = snap_raw10(frame, stride, 2 * cx + (site & 1), 2 * cy + (site >> 1));

				sum[site] += v;
				s->min = v < s->min ? v : s->min;
				s->max = v > s->max ? v : s->max;
				saturated += v >= SNAP_SATURATED;
				hash = (hash ^ v) * 16777619u;
			}
			s->samples++;
		}
	}
	for (uint32_t site = 0; site < 4; site++) {
		s->mean[site] = s->samples ? (uint32_t)(sum[site] / s->samples) : 0;
	}
	s->saturated_permille = s->samples ? (uint32_t)((uint64_t)saturated * 1000 / (4ull * s->samples)) : 0;
	s->checksum = hash;
}

/* Where red, the greens and blue sit in a 2x2 cell of each order: site numbers as in snap_stats */
static inline void
snap_sites(uint32_t bayer, uint32_t *red, uint32_t *green_a, uint32_t *green_b, uint32_t *blue)
{
	static const uint8_t sites[4][4] = {
		{ 0, 1, 2, 3 },                 /* RGGB: R (0,0), G (1,0) (0,1), B (1,1) */
		{ 1, 0, 3, 2 },                 /* GRBG */
		{ 2, 0, 3, 1 },                 /* GBRG */
		{ 3, 1, 2, 0 },                 /* BGGR */
	};
	const uint8_t *o = sites[bayer & 3];

	*red = o[0];
	*green_a = o[1];
	*green_b = o[2];
	*blue = o[3];
}

static inline void
snap_cell_rgb(const uint8_t *frame, uint32_t stride, uint32_t cx, uint32_t cy, uint32_t bayer, uint8_t rgb[3])
{
	uint32_t v[4], r, ga, gb, b;

	for (uint32_t site = 0; site < 4; site++) {
		v[site] = snap_raw10(frame, stride, 2 * cx + (site & 1), 2 * cy + (site >> 1));
	}
	snap_sites(bayer, &r, &ga, &gb, &b);
	rgb[0] = (uint8_t)(v[r] >> 2);
	rgb[1] = (uint8_t)((v[ga] + v[gb]) >> 3);
	rgb[2] = (uint8_t)(v[b] >> 2);
}

/* Row `row' of a cols x rows thumbnail as RRGGBB hex, NUL-ended; the characters written (0: `out' too small). */
static inline size_t
snap_row_hex(const uint8_t *frame, uint32_t width, uint32_t height, uint32_t stride, uint32_t bayer, uint32_t cols,
    uint32_t rows, uint32_t row, char *out, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	uint32_t cells_x = width / 2, cells_y = height / 2;
	uint32_t cy = (uint32_t)(((uint64_t)2 * row + 1) * cells_y / (2ull * rows));
	size_t at = 0;

	if (size < (size_t)cols * 6 + 1 || cols == 0 || rows == 0 || cy >= cells_y) {
		return 0;
	}
	for (uint32_t col = 0; col < cols; col++) {
		uint32_t cx = (uint32_t)(((uint64_t)2 * col + 1) * cells_x / (2ull * cols));
		uint8_t rgb[3];

		snap_cell_rgb(frame, stride, cx, cy, bayer, rgb);
		for (uint32_t k = 0; k < 3; k++) {
			out[at++] = digits[rgb[k] >> 4];
			out[at++] = digits[rgb[k] & 15];
		}
	}
	out[at] = '\0';
	return at;
}

/* `n' bytes as hex, NUL-ended; the characters written (0: `out' too small). */
static inline size_t
snap_hex(const uint8_t *bytes, size_t n, char *out, size_t size)
{
	static const char digits[] = "0123456789abcdef";

	if (size < 2 * n + 1) {
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[bytes[i] >> 4];
		out[2 * i + 1] = digits[bytes[i] & 15];
	}
	out[2 * n] = '\0';
	return 2 * n;
}

/*
 * The centre crop of a width x height frame asked as *w x *h: whole RAW10 groups across (a multiple of four pixels,
 * at a multiple of four), whole Bayer cells down (even rows, at an even row), inside the frame.
 */
static inline void
snap_crop(uint32_t width, uint32_t height, uint32_t *w, uint32_t *h, uint32_t *x, uint32_t *y)
{
	*w = (*w > width ? width : *w) & ~3u;
	*h = (*h > height ? height : *h) & ~1u;
	*x = ((width - *w) / 2) & ~3u;
	*y = ((height - *h) / 2) & ~1u;
}

/* One step towards SNAP_EXPOSURE_TARGET from a frame whose greens averaged `green'; false when it is close enough. */
static inline bool
snap_exposure_step(struct snap_exposure *e, uint32_t green)
{
	uint64_t now = (uint64_t)e->lines * e->gain, want, longest;
	uint32_t lines, gain;

	green = green == 0 ? 1 : green;
	if (green * 8 >= SNAP_EXPOSURE_TARGET * 7 && green * 8 <= SNAP_EXPOSURE_TARGET * 9) {
		return false;
	}
	want = now * SNAP_EXPOSURE_TARGET / green;
	want = want > now * SNAP_EXPOSURE_STEP_MAX ? now * SNAP_EXPOSURE_STEP_MAX : want;
	want = want < now / SNAP_EXPOSURE_STEP_MAX ? now / SNAP_EXPOSURE_STEP_MAX : want;
	longest = e->frame_lines > e->margin ? e->frame_lines - e->margin : 1;
	lines = (uint32_t)(want / e->unity > longest ? longest : want / e->unity);
	lines = lines == 0 ? 1 : lines;
	gain = (uint32_t)(want / lines);
	gain = gain < e->unity ? e->unity : gain > e->gain_max ? e->gain_max : gain;
	if (lines == e->lines && gain == e->gain) {
		return false;
	}
	e->lines = lines;
	e->gain = gain;
	return true;
}

#endif /* DLISA_CAMERA_SNAP_H */
