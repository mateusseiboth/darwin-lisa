/*
 * init26: the system's time zone from the boot-arg tz= (an IANA name, "America/Campo_Grande"), kept on the data volume.
 *
 * iOS reads its zone through /etc/localtime -> /private/var/db/timezone/localtime, a symlink to
 * /var/db/timezone/zoneinfo/<name>. The data volume's skeleton (var-skel.h) makes it US/Pacific, Cupertino's zone,
 * and nothing on this phone sets another one. With tz=NAME, init points that link at NAME at every boot (only when it
 * names another zone). CFTimeZone takes the zone's name from what follows "zoneinfo/" in the link, so the link keeps
 * the skeleton's form.
 *
 * Pure string work, no system calls: init-26.c includes it after mini.h, test/time-zone-test.c after <string.h>.
 */

#define TZ_LINK_BASE            "/var/db/timezone/zoneinfo/"
#define TZ_NAME_MOST            63

static int
isTimeZoneNameByte(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
	       c == '+' || c == '/';
}

/* NAME is a zone's name: 1 to TZ_NAME_MOST bytes of letters, digits, _ - + and /, in parts with no way up */
static int
isTimeZoneName(const char *name)
{
	size_t length = strlen(name);

	if (length == 0 || length > TZ_NAME_MOST || name[0] == '/' || name[length - 1] == '/') {
		return 0;
	}
	for (size_t at = 0; at < length; at++) {
		/* (no '.' at all: no "..", no hidden file) */
		if (!isTimeZoneNameByte(name[at]) || (name[at] == '/' && name[at + 1] == '/')) {
			return 0;
		}
	}
	return 1;
}

/* TZ_LINK_BASE and NAME into OUT (SIZE bytes, NUL-terminated); its length, or 0 when it does not fit */
static size_t
buildTimeZoneLink(char *out, size_t size, const char *name)
{
	size_t base = strlen(TZ_LINK_BASE), length = strlen(name);

	if (base + length + 1 > size) {
		return 0;
	}
	memcpy(out, TZ_LINK_BASE, base);
	memcpy(out + base, name, length + 1);
	return base + length;
}

/* the link's target (LENGTH bytes, not a string, as readlink gives it) is NAME's link already */
static int
isTimeZoneLinkTo(const char *target, size_t length, const char *name)
{
	size_t base = strlen(TZ_LINK_BASE), n = strlen(name);

	return length == base + n && memcmp(target, TZ_LINK_BASE, base) == 0 && memcmp(target + base, name, n) == 0;
}
