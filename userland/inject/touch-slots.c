/*
 * libdlisa, backboardd: the path index and identity a contact is sent with.
 *
 * Until 2026-11-13 a contact went out as path index finger+1 and identity finger+2, so the panel's lone finger (0)
 * was path 1 / identity 2 and the kernel's touch_synth finger (9) was path 10 / identity 11. Inside an app the
 * synthetic finger never did anything (Settings: the same screenshots with and without `scr`/`scrd`, runs S8-B and
 * S9-A), while the panel's finger scrolled it (planb-standalone, 27 frames a second); in SpringBoard both work. So a
 * contact now takes the lowest slot no other contact holds while it is down, and a lone finger, real or synthetic,
 * is always path 1 / identity 2, as Apple's driver sends one finger.
 */
#include <stdbool.h>
#include <stdint.h>

#define TOUCH_FINGERS           10      /* finger numbers 0..9 from /dev/hakios-touch */
#define TOUCH_SLOT_NONE         (-1)

struct touchSlots {
	int8_t slot[TOUCH_FINGERS];     /* by finger number: its slot while it is down, TOUCH_SLOT_NONE otherwise */
};

static void
initTouchSlots(struct touchSlots *slots)
{
	for (unsigned f = 0; f < TOUCH_FINGERS; f++) {
		slots->slot[f] = TOUCH_SLOT_NONE;
	}
}

static int
findTouchSlot(const struct touchSlots *slots, unsigned finger)
{
	return finger < TOUCH_FINGERS ? slots->slot[finger] : TOUCH_SLOT_NONE;
}

static bool
isTouchSlotHeld(const struct touchSlots *slots, int slot)
{
	for (unsigned f = 0; f < TOUCH_FINGERS; f++) {
		if (slots->slot[f] == slot) {
			return true;
		}
	}
	return false;
}

/* A finger lands: the lowest slot nobody holds (a finger already down keeps its own). */
static int
takeTouchSlot(struct touchSlots *slots, unsigned finger)
{
	if (finger >= TOUCH_FINGERS) {
		return TOUCH_SLOT_NONE;
	}
	if (slots->slot[finger] != TOUCH_SLOT_NONE) {
		return slots->slot[finger];
	}
	for (int slot = 0; slot < TOUCH_FINGERS; slot++) {
		if (!isTouchSlotHeld(slots, slot)) {
			slots->slot[finger] = (int8_t)slot;
			return slot;
		}
	}
	return TOUCH_SLOT_NONE;
}

static void
releaseTouchSlot(struct touchSlots *slots, unsigned finger)
{
	if (finger < TOUCH_FINGERS) {
		slots->slot[finger] = TOUCH_SLOT_NONE;
	}
}

static uint32_t
touchPathIndex(int slot)
{
	return (uint32_t)slot + 1;
}

static uint32_t
touchPathIdentity(int slot)
{
	return (uint32_t)slot + 2;
}
