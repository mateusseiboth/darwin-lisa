/*
 * /sbin/launchd for the iOS 26 (23F84) bring-up on Xiaomi lisa.
 *
 * This is the first user program the kernel runs after it mounts root-26.apfs
 * as /dev/md0.  It is static (no dyld, no libSystem: libmini talks to the
 * kernel directly), so the kernel execs it straight, the same shape HakiOS's
 * stage0 had for iOS 16 (LC_UNIXTHREAD, no LC_LOAD_DYLINKER).
 *
 * For a first boot it proves three things and stops: that the kernel can exec
 * an unmodified-ABI iOS 26 userland binary from an image our APFS driver
 * produced; that a regular file reads back; and that the root directory
 * enumerates.  If /bin/sh is present it hands over to it, otherwise it halts.
 */
#include "mini.h"
#include "var-skel.h"

static const char BANNER[] =
    "\n==== iOS 26.5.2 (23F84) userland on lisa: /sbin/launchd alive ====\n";

static void
openConsole(void)
{
	int fd = open("/dev/console", O_RDWR, 0);
	if (fd < 0) {
		return; /* nothing to say it to yet; keep going */
	}
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}
}

static void
showFile(const char *path)
{
	char buf[512];
	int fd = open(path, O_RDONLY, 0);
	if (fd < 0) {
		printf("init26: %s: %s\n", path, strerror(errno));
		return;
	}
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		printf("init26: %s:\n%s\n", path, buf);
	}
	close(fd);
}

static void
listDir(const char *path)
{
	char buf[8192];
	off_t pos = 0;
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) {
		printf("init26: cannot open %s: %s\n", path, strerror(errno));
		return;
	}
	printf("init26: contents of %s:\n", path);
	for (;;) {
		ssize_t n = getdirentries(fd, buf, sizeof(buf), &pos);
		if (n <= 0) {
			break;
		}
		for (ssize_t off = 0; off < n;) {
			struct dirent *de = (struct dirent *)(buf + off);
			if (de->d_reclen == 0) {
				break;
			}
			printf("  %c %s\n", de->d_type == DT_DIR ? 'd' : (de->d_type == DT_LNK ? 'l' : '-'), de->d_name);
			off += de->d_reclen;
		}
	}
	close(fd);
}

static void
statPath(const char *p)
{
	struct stat st;
	if (stat(p, &st) == 0)
		printf("init26: CACHE ok  %s  (%lld bytes)\n", p, (long long)st.st_size);
	else
		printf("init26: CACHE --  %s: %s\n", p, strerror(errno));
}

/* stat() follows symlinks, so the cryptex-path check tests the /System/Cryptexes/OS
 * symlink resolution AND the cache file in one call. This is why launchd (dyld) said
 * "no dyld cache": we need to know which of these the booted image actually resolves. */
static void
cacheProbe(void)
{
	printf("init26: dyld shared-cache probe:\n");
	statPath("/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e");
	statPath("/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e");
	statPath("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e");
	statPath("/System/Cryptexes/OS");
	statPath("/usr/lib/libSystem.B.dylib");
}

/* Memory headroom with the ramdisk resident: what the kernel was left with before
 * Apple's launchd starts. Values are bytes (hw.*) or 16 KB pages (vm.*). */
static void
memProbe(void)
{
	static const char *const names[] = {
		"hw.memsize", "hw.memsize_usable", "vm.pages", "vm.page_free_count",
		"vm.page_free_target", "vm.page_free_min", "kern.memorystatus_level", NULL
	};
	for (int i = 0; names[i] != NULL; i++) {
		unsigned long long v = 0;
		size_t len = sizeof(v);
		if (sysctlbyname(names[i], &v, &len, NULL, 0) != 0) {
			printf("init26: MEM %s: %s\n", names[i], strerror(errno));
			continue;
		}
		if (len == 4) {
			v &= 0xffffffffULL;
		}
		printf("init26: MEM %s = %llu\n", names[i], v);
	}
}

/*
 * The data volume. On an iPhone /private/var is the APFS Data volume, created at
 * restore from the rootfs's private/var. Here the root is a read-only image in
 * memory and nothing may be written to flash, so /private/var is ramfs (memory,
 * empty at every boot) with the data volume's directory skeleton laid down again
 * (var-skel.h, generated from the IPSW). Without it launchd and the daemons hit
 * "Read-only file system" for /private/var/mobile/tmp, /var/run, and so on.
 */
static void
mountVar(void)
{
	char path[512];
	int made = 0, bad = 0;

	if (mount("ramfs", "/private/var", 0, NULL) != 0) {
		printf("init26: VAR mount ramfs on /private/var: %s\n", strerror(errno));
		return;
	}
	for (const struct var_skel *e = VAR_SKEL; e->path != NULL; e++) {
		size_t n = strlen("/private/var") + strlen(e->path) + 1;
		if (n > sizeof(path)) {
			continue;
		}
		strcpy(path, "/private/var");
		strcpy(path + strlen("/private/var"), e->path);
		int ok;
		if (e->type == 'l') {
			ok = symlink(e->target, path) == 0 && lchown(path, e->uid, e->gid) == 0;
		} else {
			ok = mkdir(path, e->mode) == 0 && chown(path, e->uid, e->gid) == 0 &&
			    chmod(path, e->mode) == 0;
		}
		if (ok) {
			made++;
		} else {
			bad++;
			if (bad <= 5) {
				printf("init26: VAR %s: %s\n", path, strerror(errno));
			}
		}
	}
	/*
	 * logd takes a log directory without a .migrated marker for one left by an older
	 * system and asks logd_helper to convert it, which fails here and logd traps
	 * ("BUG IN LIBTRACE: Failed to migrate directory"). These are brand new: mark them
	 * (owner _logd, 272), as HakiOS's init does.
	 */
	static const char *const markers[] = { "/private/var/db/diagnostics/.migrated",
		"/private/var/db/diagnostics/timesync/.migrated", "/private/var/db/uuidtext/.migrated" };
	static const char *const logdirs[] = { "/private/var/db/diagnostics/timesync", "/private/var/db/uuidtext" };
	for (unsigned i = 0; i < sizeof(logdirs) / sizeof(logdirs[0]); i++) {
		mkdir(logdirs[i], 0755);
		chown(logdirs[i], 272, 272);
	}
	for (unsigned i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
		int fd = open(markers[i], O_WRONLY | O_CREAT, 0644);
		if (fd < 0 || chown(markers[i], 272, 272) != 0) {
			printf("init26: VAR %s: %s\n", markers[i], strerror(errno));
			bad++;
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	printf("init26: VAR /private/var is ramfs: %d entries made, %d failed\n", made, bad);
}

static void
handOff(char **envp)
{
	/* Hand pid 1 to Apple's real arm64e launchd (shipped as /sbin/launchd.darwin;
	 * our static init is /sbin/launchd). argv[0] stays /sbin/launchd so launchd
	 * sees the path it expects. The pac-pager strips PAC at page-in and miniamfi
	 * lets the ad-hoc signature pass; dyld then maps the shared cache. If it is
	 * absent, fall back to /bin/sh (neither present halts cleanly). */
	char *ld_argv[] = { "/sbin/launchd", NULL };
	char *sh_argv[] = { "/bin/sh", NULL };
	/* DYLD_SHARED_CACHE_DIR is what lets dyld find the cache at all on this boot: libignition
	 * runs the "ramdisk" boot spec, which grafts no cryptex and hands dyld no cache dir, and the
	 * fallback /System/Library/Caches/com.apple.dyld/ does not exist on iOS 26 (the cache lives
	 * in the OS cryptex). It does NOT force a private mapping: dyld 26 maps privately only for
	 * DYLD_SHARED_REGION=private; otherwise it calls shared_region_map_and_slide_2_np.
	 * Later processes reuse the mapped region (shared_region_check_np), so they need no env.
	 * No DYLD_PRINT_*: on this console they cost seconds per hundred lines and ran the
	 * boot past its time limit before launchd got going. Add them back only to debug dyld. */
	char *ld_envp[] = {
		"DYLD_SHARED_CACHE_DIR=/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld",
		NULL
	};
	const char *real = "/sbin/launchd.darwin";
	struct stat st;
	if (stat(real, &st) == 0) {
		printf("init26: handing over to Apple launchd (%s), %lld bytes\n", real, (long long)st.st_size);
		execve(real, ld_argv, ld_envp);
		printf("init26: cannot exec %s: %s\n", real, strerror(errno));
		return;
	}
	printf("init26: no %s in this image\n", real);
	if (stat(sh_argv[0], &st) != 0) {
		printf("init26: no %s either, nothing to hand over to\n", sh_argv[0]);
		return;
	}
	printf("init26: handing over to %s\n", sh_argv[0]);
	execve(sh_argv[0], sh_argv, envp);
	printf("init26: cannot exec %s: %s\n", sh_argv[0], strerror(errno));
}

int
main(int argc, char **argv, char **envp)
{
	(void)argc, (void)argv;
	openConsole();

	write(1, BANNER, sizeof(BANNER) - 1);
	printf("init26: pid=%d uid=%d argv0=%s\n", getpid(), getuid(), argv && argv[0] ? argv[0] : "(null)");

	showFile("/etc/hakios-release");
	listDir("/");
	cacheProbe();
	memProbe();
	mountVar();

	handOff(envp);

	printf("init26: first-boot checks done, halting.\n");
	reboot(0x08 /* RB_HALT */);
	return 0;
}
