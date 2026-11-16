/* Host test of bt-virtual.c (libdlisa: bluetoothd on its built-in virtual controller, boot-arg dlisa_btvirt=1) */
#include <stdio.h>

#include "bt-virtual.c"

static int failures;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static int a, b, c;

int
main(void)
{
	const char *on = "lisa=1 dlisa_btvirt=1 bt-transport-override=10";
	void *slots[4] = { &a, &b, &a, &c };

	expect(isFakedVmmQuery("bluetoothd", "kern.hv_vmm_present", on), "bluetoothd asking, boot-arg on");
	expect(!isFakedVmmQuery("SpringBoard", "kern.hv_vmm_present", on), "only bluetoothd is told it is in a VM");
	expect(!isFakedVmmQuery("bluetoothd", "kern.bootargs", on), "only that one sysctl");
	expect(!isFakedVmmQuery("bluetoothd", "kern.hv_vmm_present", "lisa=1"), "off without the boot-arg");
	expect(!isFakedVmmQuery("bluetoothd", "kern.hv_vmm_present", "dlisa_btvirt=0"), "off with dlisa_btvirt=0");
	expect(!isFakedVmmQuery("bluetoothd", NULL, on), "a NULL name is passed through");
	(void)isNamedByBootArg;         /* (name-list.c comes with bt-virtual.c) */
	expect(replacePointers(slots, 4, &a, &c) == 2, "both slots that held the old pointer are counted");
	expect(slots[0] == &c && slots[2] == &c && slots[1] == &b, "and only they are replaced");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
