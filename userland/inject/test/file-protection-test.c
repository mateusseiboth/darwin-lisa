/* Host test of file-protection.c (libdlisa in PosterBoard: what NSURLFileProtectionKey reads on the HFSX data volume) */
#include <stdio.h>

#include "file-protection.c"

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
	/* the data volume: hfs, local, no MNT_CPROTECT (what statfs gives on the phone) */
	const uint64_t hfsData = 0x00001000 /* MNT_LOCAL */ | 0x00100000 /* MNT_JOURNALED */;
	const uint64_t protectedData = hfsData | DLISA_MNT_CPROTECT;

	expect(isProtectionNoneImplied(true, true, hfsData), "no class on an unprotected volume: None");
	expect(!isProtectionNoneImplied(true, false, hfsData), "a class the system gave is kept");
	expect(!isProtectionNoneImplied(false, true, hfsData), "a failed lookup stays failed");
	expect(!isProtectionNoneImplied(true, true, protectedData), "a protected volume's nil is not None");
	expect(DLISA_MNT_CPROTECT == 0x00000080, "MNT_CPROTECT is <sys/mount.h>'s 0x80");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
