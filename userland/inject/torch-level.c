/*
 * libdlisa: Control Center's flashlight on the kernel torch (kernel/drivers/torch, torch=3). AVFlashlight's level is a
 * float 0..1; /dev/hakios-torch takes 0..1000 (thousandths of the driver's 125 mA). kern.hakios.torch_status says
 * whether the driver is serving Darwin ("level 3 ... broken 0").
 */
#ifndef DLISA_TORCH_LEVEL_C
#define DLISA_TORCH_LEVEL_C
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint32_t
torchPermilleForLevel(float level)
{
	float permille = level * 1000.0f;

	if (!(permille > 0.0f)) {               /* (NaN too) */
		return 0;
	}
	if (permille >= 1000.0f) {
		return 1000;
	}
	return permille < 1.0f ? 1 : (uint32_t)(permille + 0.5f);
}

static bool
isTorchServing(const char *status)
{
	return status != NULL && strncmp(status, "level 3 ", 8) == 0 && strstr(status, " broken 0") != NULL;
}
#endif
