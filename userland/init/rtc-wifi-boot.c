/*
 * init26: the boot's part of the clock (rtc-offset.c) and of the Wi-Fi (wifi-net.c), the system calls of both.
 * init-26.c includes it after bootArgNumber() and mountVar(), and main() calls, once /private/var is there:
 *
 *   applySavedRtcOffset()  rtc=1: the offset kept on the data volume -> kern.hakios.rtc_offset (the calendar moves)
 *   startRtcKeeper()       rtc=1: a child that saves the kernel's offset whenever it changes (the system set the time)
 *   giveWifiNetwork()      wifi=1: a RAM disk's fw/network kept on the data volume, then the kept one ->
 *                          kern.hakios.wifi_network (the radio joins it once it is up). wifi_forget=1 forgets it,
 *                          wifi_seed=0 leaves RAM disks alone
 *
 * Nothing printed here says a network's name or passphrase: lengths only.
 */

#define RTC_OFFSET_FILE         "/private/var/db/dlisa-rtc-offset"
#define RTC_KEEP_SECONDS        30
#define WIFI_NET_DIR            "/private/var/db/dlisa-wifi"
#define WIFI_NET_FILE           WIFI_NET_DIR "/network"
#define WIFI_SEED_DIR           "/private/var/run/dlisa-wifi-seed"
#define WIFI_TEXT_MOST          128
#define SYS_unmount_            159

/* PATH's first SIZE bytes into BUF: how many, or -1 */
static ssize_t
readSmallFile(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY, 0);
	ssize_t n;

	if (fd < 0) {
		return -1;
	}
	n = read(fd, buf, size);
	close(fd);
	return n;
}

/* DATA into PATH (mode MODE) through PATH.new and a rename: the whole of it or nothing. 0, or -1 */
static int
saveSmallFile(const char *path, const void *data, size_t length, int mode)
{
	char temporary[160];
	size_t n = strlen(path);
	int fd, ok;

	if (n + sizeof(".new") > sizeof(temporary)) {
		return -1;
	}
	memcpy(temporary, path, n);
	memcpy(temporary + n, ".new", sizeof(".new"));
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) {
		return -1;
	}
	ok = write(fd, data, length) == (ssize_t)length && fsync(fd) == 0;
	close(fd);
	if (ok && chmod(temporary, mode) == 0 && rename(temporary, path) == 0) {
		return 0;
	}
	unlink(temporary);
	return -1;
}

static void
wipe(void *data, size_t length)
{
	volatile char *at = data;

	while (length-- > 0) {
		*at++ = 0;
	}
}

/* ---- the clock ---- */

static int
readSavedRtcOffset(long long *offset)
{
	char text[32];
	ssize_t n = readSmallFile(RTC_OFFSET_FILE, text, sizeof(text));

	return n > 0 && parseRtcOffset(text, (size_t)n, offset);
}

static void
applySavedRtcOffset(void)
{
	long long offset;

	if (bootArgNumber("rtc", 0) == 0) {
		return;
	}
	if (!readSavedRtcOffset(&offset)) {
		printf("init26: RTC no saved offset in %s: the kernel's own stands\n", RTC_OFFSET_FILE);
		return;
	}
	if (sysctlbyname("kern.hakios.rtc_offset", NULL, NULL, &offset, sizeof(offset)) != 0) {
		printf("init26: RTC the saved offset %lld s is not the kernel's: %s\n", offset, strerror(errno));
		return;
	}
	printf("init26: RTC the saved offset %lld s is the kernel's now\n", offset);
}

/* the keeper: every RTC_KEEP_SECONDS, the kernel's offset saved when it is one to keep and not the one saved */
static void
keepRtcOffset(void)
{
	long long saved = 0, offset;
	int have = readSavedRtcOffset(&saved), source;
	char text[32];

	for (;;) {
		size_t offsetSize = sizeof(offset), sourceSize = sizeof(source), n;

		usleep(RTC_KEEP_SECONDS * 1000000U);
		if (sysctlbyname("kern.hakios.rtc_offset", &offset, &offsetSize, NULL, 0) != 0 ||
		    sysctlbyname("kern.hakios.rtc_source", &source, &sourceSize, NULL, 0) != 0 ||
		    !isRtcOffsetToKeep(source) || (have && offset == saved)) {
			continue;
		}
		n = formatRtcOffset(text, sizeof(text), offset);
		if (n > 0 && saveSmallFile(RTC_OFFSET_FILE, text, n, 0644) == 0) {
			saved = offset;
			have = 1;
			printf("init26: RTC offset %lld s saved for the next boot (source %d)\n", offset, source);
		}
	}
}

static void
startRtcKeeper(void)
{
	pid_t pid;

	if (bootArgNumber("rtc", 0) == 0) {
		return;
	}
	pid = fork();
	if (pid == 0) {
		keepRtcOffset();
		_exit(0);
	}
	if (pid > 0) {
		printf("init26: RTC the kernel's offset is saved when it changes, looked at every %d s (pid %d)\n",
		    RTC_KEEP_SECONDS, pid);
	}
}

/* ---- the Wi-Fi network ---- */

/* fw/network of a RAM disk that is not the root, kept on the data volume (the latest given wins) */
static void
seedWifiNetwork(void)
{
	static const char *const disks[] = { "/dev/md1", "/dev/md0" };
	char text[WIFI_TEXT_MOST];
	struct stat root, disk;

	if (bootArgNumber("wifi_seed", 1) == 0 || stat("/", &root) != 0) {
		return;
	}
	for (unsigned i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
		ssize_t n;

		if (stat(disks[i], &disk) != 0 || (disk.st_mode & S_IFMT) != S_IFBLK || disk.st_rdev == root.st_dev) {
			continue;
		}
		mkdir(WIFI_SEED_DIR, 0700);
		if (mount("apfs", WIFI_SEED_DIR, MNT_RDONLY, (void *)(uintptr_t)disks[i]) != 0) {
			printf("init26: WIFI %s is not an APFS image to look in: %s\n", disks[i], strerror(errno));
			continue;
		}
		n = readSmallFile(WIFI_SEED_DIR "/fw/network", text, sizeof(text));
		mini_syscall(SYS_unmount_, (long)WIFI_SEED_DIR, 0, 0, 0, 0, 0);
		if (n <= 0) {
			continue;
		}
		if (!isWifiNetworkText(text, (size_t)n)) {
			printf("init26: WIFI fw/network of %s is not a name and a passphrase (%zd bytes): not kept\n", disks[i], n);
		} else if (mkdir(WIFI_NET_DIR, 0700), saveSmallFile(WIFI_NET_FILE, text, (size_t)n, 0600) == 0) {
			printf("init26: WIFI the network of %s's fw/network is kept on the data volume (%zd bytes)\n", disks[i], n);
		} else {
			printf("init26: WIFI %s: %s\n", WIFI_NET_FILE, strerror(errno));
		}
		wipe(text, sizeof(text));
		return;
	}
}

static void
giveWifiNetwork(void)
{
	char text[WIFI_TEXT_MOST];
	ssize_t n;

	if (bootArgNumber("wifi", 0) == 0) {
		return;
	}
	if (bootArgNumber("wifi_forget", 0) == 1) {
		if (unlink(WIFI_NET_FILE) == 0) {
			printf("init26: WIFI the kept network is forgotten (wifi_forget=1)\n");
		}
		return;
	}
	seedWifiNetwork();
	n = readSmallFile(WIFI_NET_FILE, text, sizeof(text));
	if (n <= 0) {
		printf("init26: WIFI no network kept on the data volume: the radio joins what a RAM disk or Settings gives it\n");
		return;
	}
	if (!isWifiNetworkText(text, (size_t)n)) {
		printf("init26: WIFI %s is not a name and a passphrase (%zd bytes): not given\n", WIFI_NET_FILE, n);
	} else if (sysctlbyname("kern.hakios.wifi_network", NULL, NULL, text, (size_t)n) != 0) {
		printf("init26: WIFI the kept network is not the kernel's: %s\n", strerror(errno));
	} else {
		printf("init26: WIFI the kept network (%zd bytes) is the kernel's to join\n", n);
	}
	wipe(text, sizeof(text));
}
