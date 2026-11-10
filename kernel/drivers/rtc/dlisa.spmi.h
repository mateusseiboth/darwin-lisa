/*
 * darwin-lisa: one peripheral of a PMIC, through the SPMI arbiter (version 5) of the SM7325, for the drivers that
 * read or write a few of its registers (the clock, drivers/rtc/dlisa.rtc.c; the battery's ADC has its own copy of the
 * same code in drivers/battery/dlisa.battery.c until it moves onto this one).
 *
 * A peripheral is named by its PPID, (slave << 8) | peripheral: 0x061 is the PMK8350's clock (slave 0, registers
 * 0x6100), 0x031 its ADC. Reads go through the observer channel, which answers for any peripheral; writes only through
 * the peripheral's own channel, and only if the arbiter says this system (EE 0, the device tree's qcom,ee) owns it.
 * One transfer at a time for the whole system: hakios_spmi_lock() (bsd/dev/hakios.platform.c).
 */
#ifndef _SYS_DLISA_SPMI_H_
#define _SYS_DLISA_SPMI_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef KERNEL
struct dlisa_spmi_peripheral {
	uint16_t ppid;                  /* (slave << 8) | peripheral */
	uint16_t channel;               /* the arbiter's channel for it */
	bool writable;                  /* the arbiter lets this system write it */
	uintptr_t observer, writer;     /* the channel's registers, mapped */
};

/* The peripheral's channel, found in the arbiter's table. NULL, or why not (the arbiter is not the one known here...). */
const char *dlisa_spmi_find(uint16_t ppid, struct dlisa_spmi_peripheral *peripheral);
/* `count` (1..8) registers from `reg` (the peripheral's low byte) on. NULL, or what went wrong. */
const char *dlisa_spmi_read(const struct dlisa_spmi_peripheral *peripheral, uint8_t reg, uint8_t *bytes, uint32_t count);
/* `count` (1..4) registers from `reg` on; refused when the channel is not this system's. NULL, or what went wrong. */
const char *dlisa_spmi_write(const struct dlisa_spmi_peripheral *peripheral, uint8_t reg, const uint8_t *bytes, uint32_t count);
#endif

#endif
