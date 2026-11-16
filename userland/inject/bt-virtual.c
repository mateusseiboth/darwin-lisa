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
#include <stdint.h>
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

/*
 * The answer bluetoothd 23F84 really uses: its getter (vtable slot 0x118 of the platform object, 0x1004ad880 unslid)
 * returns a byte cached by a dispatch_once (token 0x100b59c28, byte 0x100b59c20) whose block asks sysctlbyname
 * "kern.hv_vmm_present". On the phone (periph-p3) the rebound import was never asked: "No bluetooth on this device"
 * (BD address all 0s, not a VM). So the cache is also preset: the token done (~0), the byte 1. Only when the getter's
 * code is exactly 23F84's (its seven instructions, the same whatever the slide: adrp is page-relative).
 */
#define BLUETOOTHD_VMM_GETTER   0x1004ad880ULL
#define BLUETOOTHD_VMM_ONCE     0x100b59c28ULL
#define BLUETOOTHD_VMM_FLAG     0x100b59c20ULL

static const uint32_t bluetoothdVmmGetter[7] = {
	0x90003568, 0xf9461508, 0xb100051f, 0x54000081, 0x90003568, 0x39708100, 0xd65f03c0,
};

static bool
isKnownVmmGetter(const uint32_t *code)
{
	return memcmp(code, bluetoothdVmmGetter, sizeof(bluetoothdVmmGetter)) == 0;
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
