/*
 * libdlisa: the display's brightness as the panel's level (kernel kern.hakios.brightness, DCS 0x51, boot-arg bright=1).
 * The panel's levels 1..2047 are its normal range, and an AMOLED's level is about linear in light (nits); 2048..4095 is
 * its high brightness mode, which nothing here asks for. iOS's slider (0..1) is perceptual: the level goes with its
 * square, so half the slider is a quarter of the light, as the eye takes it.
 */
#ifndef DLISA_BRIGHTNESS_LEVEL_C
#define DLISA_BRIGHTNESS_LEVEL_C
#include <stdint.h>

#define BRIGHTNESS_LEVEL_MIN 1
#define BRIGHTNESS_LEVEL_MAX 2047

static int
brightnessLevelForFraction(float fraction)
{
	if (!(fraction > 0.0f)) {               /* (NaN too) */
		return BRIGHTNESS_LEVEL_MIN;
	}
	if (fraction >= 1.0f) {
		return BRIGHTNESS_LEVEL_MAX;
	}
	return BRIGHTNESS_LEVEL_MIN + (int)(fraction * (float)(BRIGHTNESS_LEVEL_MAX - BRIGHTNESS_LEVEL_MIN) + 0.5f);
}

static int
brightnessLevelForSlider(float slider)
{
	return brightnessLevelForFraction(slider > 0.0f ? slider * slider : slider);
}

static int
brightnessLevelForNits(float nits, float minimumNits, float maximumNits)
{
	return brightnessLevelForFraction((nits - minimumNits) / (maximumNits - minimumNits));
}
#endif
