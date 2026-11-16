/*
 * darwin-lisa: the small firmware files of the peripherals (the vibration motor's waveforms, Bluetooth's rampatch and
 * NVM), for the drivers that need them: dlisa_periph_file(name, &size).
 *
 * Looked for, in order:
 *   1. fw/<name> of the RAM disk or the flash root's image (hakios_image_file(), as the Wi-Fi driver reads its own);
 *   2. built into the kernel: bsd/dev/dlisa.periph_fw.h, made by kernel/drivers/periph-fw/periph-fw.py from the blobs
 *      on this PC (the stock vendor image, pmOS's /lib/firmware). They are proprietary: the header is made in the
 *      build tree only, never in git. Without it the kernel builds and the drivers say the file is missing.
 */
#include <libkern/libkern.h>
#include <sys/systm.h>

#if __has_include("dlisa.periph_fw.h")
#include "dlisa.periph_fw.h"
#else
struct dlisa_periph_blob {
	const char *name;
	const uint8_t *data;
	uint32_t size;
};
static const struct dlisa_periph_blob dlisa_periph_blobs[] = { { NULL, NULL, 0 } };
#endif

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
const uint8_t *dlisa_periph_file(const char *name, uint64_t *size);

const uint8_t *
dlisa_periph_file(const char *name, uint64_t *size)
{
	const uint8_t *data = hakios_image_file("fw", name, size, NULL);

	if (data != NULL) {
		return data;
	}
	for (size_t i = 0; i < sizeof(dlisa_periph_blobs) / sizeof(dlisa_periph_blobs[0]); i++) {
		if (dlisa_periph_blobs[i].name != NULL && strcmp(dlisa_periph_blobs[i].name, name) == 0) {
			*size = dlisa_periph_blobs[i].size;
			return dlisa_periph_blobs[i].data;
		}
	}
	*size = 0;
	return NULL;
}
