/* Host test of touch-slots.c (libdlisa in backboardd: the path index and identity a contact is sent with) */
#include <stdio.h>

#include "touch-slots.c"

static int failures;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

int
main(void)
{
	struct touchSlots slots;

	initTouchSlots(&slots);
	expect(findTouchSlot(&slots, 0) == TOUCH_SLOT_NONE, "no finger has a slot before it lands");

	/* the panel's one finger keeps what it always had: path index 1, identity 2 */
	expect(takeTouchSlot(&slots, 0) == 0, "the panel's finger 0 takes slot 0");
	expect(touchPathIndex(0) == 1 && touchPathIdentity(0) == 2, "slot 0 is path index 1, identity 2");
	expect(takeTouchSlot(&slots, 0) == 0, "a finger already down keeps its slot");
	releaseTouchSlot(&slots, 0);
	expect(findTouchSlot(&slots, 0) == TOUCH_SLOT_NONE, "a lifted finger lets its slot go");

	/* the synthetic finger (9) alone is sent exactly as a real lone finger, not as path 10 / identity 11 */
	expect(takeTouchSlot(&slots, 9) == 0, "the synthetic finger 9 alone takes slot 0");
	expect(touchPathIndex(findTouchSlot(&slots, 9)) == 1, "so its path index is 1");
	expect(touchPathIdentity(findTouchSlot(&slots, 9)) == 2, "and its identity 2");

	/* a second contact while the first is down gets the next free slot */
	expect(takeTouchSlot(&slots, 0) == 1, "a real finger landing beside it takes slot 1");
	releaseTouchSlot(&slots, 9);
	expect(findTouchSlot(&slots, 0) == 1, "the other contact keeps its slot when one lifts");
	expect(takeTouchSlot(&slots, 3) == 0, "the lowest free slot is taken again");

	/* every finger number the driver can send fits, and a bad one is refused */
	initTouchSlots(&slots);
	for (unsigned f = 0; f < TOUCH_FINGERS; f++) {
		expect(takeTouchSlot(&slots, f) == (int)f, "ten contacts take ten slots");
	}
	expect(takeTouchSlot(&slots, TOUCH_FINGERS) == TOUCH_SLOT_NONE, "a finger number past the table is refused");
	releaseTouchSlot(&slots, TOUCH_FINGERS);   /* and lifting it does nothing */
	expect(findTouchSlot(&slots, TOUCH_FINGERS) == TOUCH_SLOT_NONE, "a finger number past the table has no slot");

	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
