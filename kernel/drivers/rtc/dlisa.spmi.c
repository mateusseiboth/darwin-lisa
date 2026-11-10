/*
 * darwin-lisa: a PMIC peripheral's registers through the SPMI arbiter v5 (sys/dlisa.spmi.h).
 *
 * The same arbiter access as drivers/battery/dlisa.battery.c (which proved it on the phone: version 0x50020000, 512
 * channels, the ADC on channel 101 of system 0), made shareable: the battery driver keeps its own copy until it moves
 * onto this file. Addresses from Linux drivers/spmi/spmi-pmic-arb.c and the lisa device tree (spmi@c440000:
 * core 0x0c440000, chnls 0x0c600000, obsrvr 0x0e600000, intr 0x0e600000, cnfg 0x0c40a000, qcom,ee 0, qcom,channel 0).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/dlisa.spmi.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* reads: 0x80 per channel, for our system (the first) */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL    /* writes: 0x10000 per channel */
#define ARB_SYSTEM              0               /* the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000

static LCK_GRP_DECLARE(dlisa_spmi_lock_group, "dlisa-spmi");
static LCK_MTX_DECLARE(dlisa_spmi_map_lock, &dlisa_spmi_lock_group);
static vm_offset_t arb_core, arb_cnfg, arb_observer;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
map_arbiter(void)
{
	lck_mtx_lock(&dlisa_spmi_map_lock);
	if (arb_core == 0) {
		arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
		arb_observer = ml_io_map(ARB_OBSERVER_PHYS + 0x10000UL * ARB_SYSTEM, ARB_OBSERVER_SIZE);
		arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);     /* (last: the others are there once it is) */
	}
	lck_mtx_unlock(&dlisa_spmi_map_lock);
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	}
	if (status & STATUS_DENIED) {
		return "the arbiter refuses";
	}
	if (status & STATUS_FAILURE) {
		return "the transfer failed";
	}
	if (status & STATUS_DROPPED) {
		return "the transfer was dropped";
	}
	return NULL;
}

const char *
dlisa_spmi_find(uint16_t ppid, struct dlisa_spmi_peripheral *peripheral)
{
	uint32_t arbiter_version, channel_count;
	bool found = false;

	map_arbiter();
	arbiter_version = rd(arb_core + ARB_VERSION);
	channel_count = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (arbiter_version < 0x50000000u || arbiter_version >= 0x70000000u) {
		return "not the arbiter version 5 this code knows";
	}
	channel_count = channel_count > ARB_MAX_CHANNELS ? ARB_MAX_CHANNELS : channel_count;
	for (uint32_t n = 0; n < channel_count; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));
		bool ours;

		if (map == 0 || ((map >> 8) & 0xfff) != ppid) {
			continue;
		}
		ours = (rd(arb_cnfg + ARB_OWNER(n)) & 7) == ARB_SYSTEM;
		/* (the channel this system may write wins over one it may only read) */
		if (!found || ours) {
			found = true;
			peripheral->channel = (uint16_t)n;
			peripheral->writable = ours;
		}
	}
	if (!found) {
		return "no channel of the arbiter is that peripheral";
	}
	peripheral->ppid = ppid;
	peripheral->observer = arb_observer + 0x80UL * peripheral->channel;
	peripheral->writer = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * peripheral->channel, 0x1000);
	return NULL;
}

const char *
dlisa_spmi_read(const struct dlisa_spmi_peripheral *peripheral, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = peripheral->observer;
	uint32_t data[2];
	const char *problem;

	if (count == 0 || count > 8 || channel == 0) {
		return "not a read this code makes";
	}
	hakios_spmi_lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	hakios_spmi_unlock();
	for (uint32_t i = 0; problem == NULL && i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return problem;
}

const char *
dlisa_spmi_write(const struct dlisa_spmi_peripheral *peripheral, uint8_t reg, const uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = peripheral->writer;
	uint32_t word = 0;
	const char *problem = NULL;

	if (!peripheral->writable || channel == 0) {
		return "the peripheral's channel is not this system's to write";
	}
	if (count == 0 || count > 4) {
		return "not a write this code makes";
	}
	for (uint32_t i = 0; i < count; i++) {
		word |= (uint32_t)bytes[i] << (8 * i);
	}
	for (int tries = 0; tries < 3; tries++) {
		hakios_spmi_lock();
		wr(channel + CHANNEL_WDATA0, word);
		wr(channel + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4 | (count - 1));
		problem = wait_done(channel);
		hakios_spmi_unlock();
		if (problem == NULL || strcmp(problem, "the transfer was dropped") != 0) {
			break;
		}
	}
	return problem;
}
