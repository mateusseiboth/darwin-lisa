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
	/* Point dyld straight at the shared cache (the cryptex is not activated in this
	 * ramdisk boot, so dyld's normal cryptex lookup finds nothing). DYLD_PRINT_SEARCHING
	 * doubles as a probe: if it logs, dyld honors our env (launchd is not a locked-down
	 * platform binary here); if it is silent, miniamfi's CS_PLATFORM_BINARY is blocking it. */
	char *ld_envp[] = {
		"DYLD_SHARED_CACHE_DIR=/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld",
		"DYLD_PRINT_SEARCHING=1",
		"DYLD_PRINT_LIBRARIES=1",
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

	handOff(envp);

	printf("init26: first-boot checks done, halting.\n");
	reboot(0x08 /* RB_HALT */);
	return 0;
}
