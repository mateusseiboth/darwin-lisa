#include <math.h>
#include <stdio.h>

#include "brightness-level.c"

static int fails;

static void
expect(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	fails += !ok;
}

int
main(void)
{
	expect(brightnessLevelForSlider(0.0f) == BRIGHTNESS_LEVEL_MIN, "slider 0: the dimmest level, never black");
	expect(brightnessLevelForSlider(-1.0f) == BRIGHTNESS_LEVEL_MIN, "below 0: the dimmest level");
	expect(brightnessLevelForSlider(NAN) == BRIGHTNESS_LEVEL_MIN, "NaN: the dimmest level");
	expect(brightnessLevelForSlider(1.0f) == BRIGHTNESS_LEVEL_MAX, "slider 1: 2047 (the normal maximum)");
	expect(brightnessLevelForSlider(7.0f) == BRIGHTNESS_LEVEL_MAX, "above 1: still 2047, never the HBM range");
	expect(brightnessLevelForSlider(0.5f) > 300 && brightnessLevelForSlider(0.5f) < 600,
	    "half the slider: a quarter of the levels or so (the eye's curve)");
	expect(brightnessLevelForSlider(0.25f) < brightnessLevelForSlider(0.5f) &&
	    brightnessLevelForSlider(0.5f) < brightnessLevelForSlider(0.75f), "the level grows with the slider");
	expect(brightnessLevelForNits(0.0f, 2.0f, 500.0f) == BRIGHTNESS_LEVEL_MIN, "0 nits: the dimmest level");
	expect(brightnessLevelForNits(500.0f, 2.0f, 500.0f) == BRIGHTNESS_LEVEL_MAX, "the maximum nits: 2047");
	expect(brightnessLevelForNits(251.0f, 2.0f, 500.0f) > 1000 && brightnessLevelForNits(251.0f, 2.0f, 500.0f) < 1048,
	    "nits are linear in the level (an AMOLED's DBV)");
	printf("%d failures\n", fails);
	return fails != 0;
}
