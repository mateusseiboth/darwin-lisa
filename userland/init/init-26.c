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
handOff(char **envp)
{
	char *sh_argv[] = { "/bin/sh", NULL };
	struct stat st;
	if (stat(sh_argv[0], &st) != 0) {
		printf("init26: no %s in this image, nothing to hand over to\n", sh_argv[0]);
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

	handOff(envp);

	printf("init26: first-boot checks done, halting.\n");
	reboot(0x08 /* RB_HALT */);
	return 0;
}
