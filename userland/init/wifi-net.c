/*
 * init26: the Wi-Fi network the kernel joins by itself (boot-arg wifi=1, kernel/drivers/wifi-join), and wifid only on
 * a Wi-Fi boot.
 *
 * The network: /private/var/db/dlisa-wifi/network on the data volume (mode 0600), the name on its first line and the
 * passphrase on its second (none: an open network) -- the same text as fw/network of a RAM disk, which the kernel
 * reads itself when a host run uploads one. init26 copies a RAM disk's fw/network onto the data volume once (so that
 * later boots without a computer join too), and gives the kept one to the kernel through the write-only sysctl
 * kern.hakios.wifi_network. Nothing here or in the kernel ever prints a name or a passphrase: lengths only.
 *
 * wifid: its LaunchDaemon is in the image, but it is left out of the boot (launchd's disabled.plist, as ld_disable=
 * does it) unless the boot-args say wifi=N with N > 0, and wifid=0 leaves it out even then (a kernel-only join test).
 *
 * Pure string work, no system calls: init-26.c includes it after mini.h, test/wifi-net-test.c after <string.h>.
 */

#define WIFI_NAME_MOST          32
#define WIFI_PASSPHRASE_LEAST   8
#define WIFI_PASSPHRASE_MOST    63

/* The line at TEXT (LENGTH bytes left) without its line end; *next: where the one after it begins */
static size_t
measureWifiLine(const char *text, size_t length, size_t *next)
{
	size_t end = 0, line;

	while (end < length && text[end] != '\n') {
		end++;
	}
	line = end > 0 && text[end - 1] == '\r' ? end - 1 : end;
	*next = end < length ? end + 1 : end;
	return line;
}

/* TEXT (LENGTH bytes) is a network: a name of 1..32 bytes, then a passphrase of 8..63 bytes or none, and nothing more */
static int
isWifiNetworkText(const char *text, size_t length)
{
	size_t afterName = 0, afterPassphrase = 0;
	size_t name = measureWifiLine(text, length, &afterName);
	size_t passphrase = measureWifiLine(text + afterName, length - afterName, &afterPassphrase);

	if (name == 0 || name > WIFI_NAME_MOST) {
		return 0;
	}
	if (passphrase != 0 && (passphrase < WIFI_PASSPHRASE_LEAST || passphrase > WIFI_PASSPHRASE_MOST)) {
		return 0;
	}
	return afterName + afterPassphrase == length;
}

/* the value of boot-arg NAME (with its "=", a whole word of the boot-args), or NULL */
static const char *
findWifiArg(const char *bootArgs, const char *name)
{
	size_t n = strlen(name);

	for (const char *p = bootArgs; *p != 0; p++) {
		if ((p == bootArgs || p[-1] == ' ') && strncmp(p, name, n) == 0) {
			return p + n;
		}
	}
	return NULL;
}

static int
isWifidWanted(const char *bootArgs)
{
	const char *wifi = findWifiArg(bootArgs, "wifi="), *wifid = findWifiArg(bootArgs, "wifid=");

	return wifi != NULL && *wifi >= '1' && *wifi <= '9' && !(wifid != NULL && *wifid == '0');
}

/*
 * The boot-args as init26's ld_disable= is to read them (ld-disable.c): the same, or with wifid in front of the
 * ld_disable= list (a new ld_disable=wifid at the end when there is none). OUT holds the changed text; when it has no
 * room the boot-args come back as they were.
 */
static const char *
buildWifiBootArgs(const char *bootArgs, char *out, size_t size)
{
	static const char key[] = "ld_disable=", token[] = "wifid";
	const char *list = findWifiArg(bootArgs, key);
	size_t length = strlen(bootArgs), head;

	if (isWifidWanted(bootArgs) || length + sizeof(key) + sizeof(token) + 1 > size) {
		return bootArgs;
	}
	if (list != NULL) {
		head = (size_t)(list - bootArgs);
		memcpy(out, bootArgs, head);
		memcpy(out + head, token, sizeof(token) - 1);
		out[head + sizeof(token) - 1] = ',';
		strcpy(out + head + sizeof(token), list);
		return out;
	}
	strcpy(out, bootArgs);
	if (length > 0) {
		out[length++] = ' ';
	}
	strcpy(out + length, key);
	strcpy(out + length + sizeof(key) - 1, token);
	return out;
}

/* ... with a buffer of its own: what saveDisabledServices() hands buildDisabledPlist() */
static const char *
withWifiServices(const char *bootArgs)
{
	static char out[1100];

	return buildWifiBootArgs(bootArgs, out, sizeof(out));
}
