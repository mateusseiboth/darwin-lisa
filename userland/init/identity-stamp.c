/*
 * init26: the phone's identity as the loader gives it (sysctl hw.machine and hw.model, from the device tree), kept on
 * the data volume. MobileGestalt caches what it works out from the identity (screen class, model, capabilities) in
 * /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/.../com.apple.MobileGestalt.plist,
 * and only rebuilds it when the build changes. When the loader presents another iPhone, init removes that cache.
 *
 * Pure string work: init-26.c includes it after mini.h, test/identity-stamp-test.c after <string.h>.
 */

/* "MACHINE MODEL\n" into out (size bytes), cut to fit */
static void
buildIdentityStamp(char *out, size_t size, const char *machine, const char *model)
{
	size_t at = 0;
	const char *parts[] = { machine, " ", model, "\n" };

	for (unsigned p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
		for (const char *c = parts[p]; *c != 0 && at + 1 < size; c++) {
			out[at++] = *c;
		}
	}
	out[at] = 0;
}

/* the kept stamp (length bytes, not a string) is not this one */
static int
isIdentityChanged(const char *kept, size_t length, const char *current)
{
	return length != strlen(current) || memcmp(kept, current, length) != 0;
}
