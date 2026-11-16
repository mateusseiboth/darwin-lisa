/*
 * libdlisa: bluetoothd on its own virtual controller (boot-arg dlisa_btvirt=1, with bt-transport-override=10).
 *
 * bluetoothd 23F84 has a complete software controller (transport 10, chipset APPLE_VIRTUAL 6000, BTVCHciManager)
 * that it uses inside a virtual machine: when sysctl kern.hv_vmm_present is non-zero it skips its serial port check,
 * builds that controller and never asks BlueTool to boot a Broadcom chip. bluetoothd alone is told it runs in a VM;
 * the rest of the system is not. Settings and Control Center then see Bluetooth powered on, with no radio behind it
 * (docs/peripherals.md, "Bluetooth").
 */
#ifndef DLISA_BT_VIRTUAL_C
#define DLISA_BT_VIRTUAL_C
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "name-list.c"

static bool
isFakedVmmQuery(const char *program, const char *name, const char *bootArgs)
{
	const char *flag = bootArgs != NULL ? findBootArgValue(bootArgs, "dlisa_btvirt") : NULL;

	if (program == NULL || name == NULL || flag == NULL) {
		return false;
	}
	return strcmp(program, "bluetoothd") == 0 && strcmp(name, "kern.hv_vmm_present") == 0 && strtol(flag, NULL, 0) != 0;
}

/* every slot of `slots` that holds `old` now holds `replacement`; how many did */
static unsigned
replacePointers(void **slots, size_t count, void *old, void *replacement)
{
	unsigned replaced = 0;

	for (size_t i = 0; i < count; i++) {
		if (slots[i] != old) {
			continue;
		}
		slots[i] = replacement;
		replaced++;
	}
	return replaced;
}
#endif
