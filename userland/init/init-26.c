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
#include "ld-disable.c"
#include "identity-stamp.c"
#include "rtc-offset.c"
#include "wifi-net.c"

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

/* kern.bootargs, read once */
static const char *
bootArgs(void)
{
	static char args[1024];
	static int read_once;
	size_t len = sizeof(args) - 1;

	if (!read_once) {
		read_once = 1;
		if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
			len = 0;
		}
		args[len] = 0;
	}
	return args;
}

/* the value of boot-arg NAME= as a number (a whole word of the line); dflt when it is not there */
static unsigned
bootArgNumber(const char *name, unsigned dflt)
{
	const char *a = bootArgs();
	size_t n = strlen(name);

	for (const char *p = a; *p != 0; p++) {
		if ((p == a || p[-1] == ' ') && strncmp(p, name, n) == 0 && p[n] == '=') {
			unsigned v = 0;

			for (p += n + 1; *p >= '0' && *p <= '9'; p++) {
				v = v * 10 + (unsigned)(*p - '0');
			}
			return v;
		}
	}
	return dflt;
}

/*
 * The data volume on the phone's flash (boot-arg var_ufs=1): /dev/ufs_data, the region the loader and the kernel
 * offer for writes (ufs_write=1 ufs_data=NAME; docs/data-volume.md), journaled case-sensitive HFS Plus. What
 * Darwin keeps in /private/var is then there at the next boot. If it does not mount, ramfs as before.
 */
#define VAR_DEV                 "/dev/ufs_data"
#define DKIOCGETBLOCKSIZE       0x40046418UL    /* _IOR('d', 24, uint32_t) */
#define DKIOCGETBLOCKCOUNT      0x40086419UL    /* _IOR('d', 25, uint64_t) */
#define HFSIOC_RESIZE_VOLUME    0x80086802UL    /* _IOW('h', 2, u_int64_t), bsd/hfs/hfs_fsctl.h */
#define SYS_sync_               36
#define SYS_fsctl_              242

/* struct hfs_mount_args as user space passes it (bsd/hfs/hfs_mount.h; HakiOS's init the same) */
struct hfs_args {
	const char *fspec;
	uint32_t uid, gid;
	uint16_t mask, pad;
	uint32_t encoding;
	int tz[2];
	int flags, journal_tbuffer_size, journal_flags, journal_disable;
};

/* the device's bytes (0: not known), asked before the mount: a mounted block device is not opened again */
static uint64_t
varDeviceBytes(void)
{
	uint32_t bs = 0;
	uint64_t count = 0;
	int fd = open(VAR_DEV, O_RDONLY, 0);

	if (fd < 0) {
		return 0;
	}
	if (mini_syscall(SYS_ioctl, fd, (long)DKIOCGETBLOCKSIZE, (long)&bs, 0, 0, 0) != 0 ||
	    mini_syscall(SYS_ioctl, fd, (long)DKIOCGETBLOCKCOUNT, (long)&count, 0, 0, 0) != 0) {
		bs = 0;
	}
	close(fd);
	return (uint64_t)bs * count;
}

/*
 * var_grow=1: the volume grows to the whole device (HFS's own resize, hfs_extendfs). For the move from option A to
 * option B: the 4 GiB volume copied block for block to the start of a bigger partition, then grown here once.
 */
static void
growVar(uint64_t device)
{
	uint64_t want = device;

	if (device == 0) {
		printf("init26: VAR var_grow=1: the size of %s is not known: not grown\n", VAR_DEV);
		return;
	}
	if (mini_syscall(SYS_fsctl_, (long)"/private/var", (long)HFSIOC_RESIZE_VOLUME, (long)&want, 0, 0, 0) != 0) {
		printf("init26: VAR var_grow=1: resize to %llu bytes: %s\n", (unsigned long long)device, strerror(errno));
		return;
	}
	printf("init26: VAR the data volume is now %llu MB (the whole of %s)\n", (unsigned long long)(device >> 20),
	    VAR_DEV);
}

static int
mountFlashVar(void)
{
	struct hfs_args args;
	struct stat st;
	uint64_t device;

	if (bootArgNumber("var_ufs", 0) != 1) {
		return 0;
	}
	if (stat(VAR_DEV, &st) != 0) {
		printf("init26: VAR var_ufs=1 but no %s (ufs_write=1 ufs_data=NAME, and a loader with that region): ramfs\n",
		    VAR_DEV);
		return 0;
	}
	device = varDeviceBytes();
	memset(&args, 0, sizeof(args));
	args.fspec = VAR_DEV;
	if (mount("hfs", "/private/var", 0, &args) != 0) {
		printf("init26: VAR HFS Plus from %s (%llu MB): %s; ramfs instead, empty at every boot\n", VAR_DEV,
		    (unsigned long long)(device >> 20), strerror(errno));
		return 0;
	}
	printf("init26: VAR /private/var is HFS Plus from %s (%llu MB), kept between boots\n", VAR_DEV,
	    (unsigned long long)(device >> 20));
	if (bootArgNumber("var_grow", 0) == 1) {
		growVar(device);
	}
	return 1;
}

/* What is under PATH, gone; PATH itself stays (a kept volume's tmp, run and vm are each boot's own; HakiOS's) */
static void
emptyTree(const char *path, int depth)
{
	char buf[4096], child[1100];
	off_t position = 0;
	ssize_t n;
	struct stat st;
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0), passes = 0;

	if (fd < 0 || depth > 32) {
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	/* what is removed moves the rest, so the directory is read again from its start until it is empty (or stuck) */
	while (passes++ < 64 && (n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
			const struct dirent *de = (const struct dirent *)(buf + at);
			size_t lp = strlen(path), ln = strlen(de->d_name);

			if (de->d_reclen == 0) {
				break;
			}
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 || lp + ln + 2 > sizeof(child)) {
				continue;
			}
			memcpy(child, path, lp);
			child[lp] = '/';
			memcpy(child + lp + 1, de->d_name, ln + 1);
			if (lstat(child, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
				emptyTree(child, depth + 1);
				rmdir(child);
			} else {
				unlink(child);
			}
		}
		position = 0;
		lseek(fd, 0, SEEK_SET);
	}
	close(fd);
}

/*
 * var_sync=N (default 30 with the volume on the flash; 0: never): a child of init that calls sync() every N seconds,
 * so that a run that ends in a reset (the usual end here: no orderly shutdown reaches the kernel) loses at most N
 * seconds of what was written. HFS's journal keeps the volume whole either way.
 */
static void
startVarSync(void)
{
	unsigned secs = bootArgNumber("var_sync", 30);
	pid_t pid;

	if (secs == 0) {
		return;
	}
	pid = fork();
	if (pid == 0) {
		for (;;) {
			usleep(secs * 1000000U);
			mini_syscall(SYS_sync_, 0, 0, 0, 0, 0, 0);
		}
	}
	if (pid > 0) {
		printf("init26: VAR sync every %u s (pid %d)\n", secs, pid);
	}
}

/* the system's build (ProductBuildVersion in SystemVersion.plist) into BUF; "23F84" when it cannot be read */
static void
readBuildVersion(char *buf, size_t size)
{
	static const char key[] = "<key>ProductBuildVersion</key>";
	static char plist[4096];
	int fd = open("/System/Library/CoreServices/SystemVersion.plist", O_RDONLY, 0);
	ssize_t n = fd >= 0 ? read(fd, plist, sizeof(plist) - 1) : -1;

	strcpy(buf, "23F84");
	if (fd >= 0) {
		close(fd);
	}
	if (n <= 0) {
		return;
	}
	plist[n] = 0;
	for (char *p = plist; *p != 0; p++) {
		if (strncmp(p, key, sizeof(key) - 1) != 0) {
			continue;
		}
		for (p += sizeof(key) - 1; *p != 0 && *p != '>'; p++) {
		}
		if (*p == 0) {
			return;
		}
		size_t len = 0;
		for (p++; p[len] != 0 && p[len] != '<' && len + 1 < size; len++) {
		}
		if (len > 0) {
			memcpy(buf, p, len);
			buf[len] = 0;
		}
		return;
	}
}

/*
 * DataMigrator (com.apple.datamigrator, the progress bar under the Apple logo) migrates the data volume when the
 * build it last migrated, LastSystemVersion in the com.apple.migration preferences, is not this one, or when the
 * sentinel /private/var/datamigrator/migrate is there. On a new /private/var that is every boot, and here its plugins
 * cannot run (com.apple.migrationpluginwrapper aborts and launchd throttles it), so it waits out each plugin's
 * watchdog: about 20 minutes, never finishing. A restored phone has the record; write it at every boot, for root (the
 * migrator's own user), mobile and the any-user domain, and drop the sentinel and the migrator's context. var_migrate=1: leave both.
 * Returns the number of failures.
 */
static int
markMigrationDone(void)
{
	static const struct {
		const char *path;
		int uid;
	} prefs[] = {
		{ "/private/var/root/Library/Preferences/com.apple.migration.plist", 0 },
		{ "/private/var/mobile/Library/Preferences/com.apple.migration.plist", 501 },
		{ "/private/var/preferences/com.apple.migration.plist", 0 },
	};
	char build[32], plist[1024];
	int bad = 0, made = 0;

	if (bootArgNumber("var_migrate", 0) == 1) {
		printf("init26: VAR var_migrate=1: DataMigrator's record is left as it is\n");
		return 0;
	}
	readBuildVersion(build, sizeof(build));
	strcpy(plist, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n<dict>\n\t<key>LastSystemVersion</key>\n\t<string>");
	strcpy(plist + strlen(plist), build);
	strcpy(plist + strlen(plist), "</string>\n\t<key>DMLastMigrationResults</key>\n\t<dict>\n\t\t<key>buildVersion</key>\n\t\t<string>");
	strcpy(plist + strlen(plist), build);
	strcpy(plist + strlen(plist), "</string>\n\t\t<key>success</key>\n\t\t<true/>\n\t</dict>\n</dict>\n</plist>\n");
	for (unsigned i = 0; i < sizeof(prefs) / sizeof(prefs[0]); i++) {
		/* the migrator writes this domain itself as it starts (its reboot count): written over at every boot */
		int fd = open(prefs[i].path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		size_t len = strlen(plist);
		if (fd < 0 || write(fd, plist, len) != (ssize_t)len || chown(prefs[i].path, prefs[i].uid, prefs[i].uid) != 0) {
			printf("init26: VAR %s: %s\n", prefs[i].path, strerror(errno));
			bad++;
		} else {
			made++;
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	if (unlink("/private/var/datamigrator/migrate") == 0) {
		printf("init26: VAR DataMigrator's sentinel removed\n");
	}
	/* its context (the user data disposition of a migration asked for on the next reboot) */
	if (unlink("/private/var/mobile/Library/Preferences/com.apple.DataMigration.plist") == 0) {
		printf("init26: VAR DataMigrator's context removed\n");
	}
	printf("init26: VAR DataMigrator: build %s recorded as migrated (%d written, %d failed)\n", build, made, bad);
	return bad;
}

/*
 * Boot-arg ld_disable=: launchd's disabled-services file, written at every boot (ld-disable.c says what goes in it).
 * Returns the number of failures.
 */
#define LD_DISABLE_DIR          "/private/var/db/com.apple.xpc.launchd"
#define LD_DISABLE_PLIST        LD_DISABLE_DIR "/disabled.plist"
#define LD_DISABLE_GROUPS       "/etc/dlisa-ld-groups.conf"

static int
saveDisabledServices(void)
{
	static char groups[16384], plist[16384];
	int fd = open(LD_DISABLE_GROUPS, O_RDONLY, 0);
	ssize_t n = fd < 0 ? 0 : read(fd, groups, sizeof(groups) - 1);
	int count;

	if (fd >= 0) {
		close(fd);
	}
	groups[n > 0 ? n : 0] = 0;
	count = buildDisabledPlist(withWifiServices(bootArgs()), n > 0 ? groups : NULL, plist, sizeof(plist));
	if (count < 0) {
		printf("init26: VAR ld_disable: the list does not fit in %zu bytes; disabled.plist left as it is\n", sizeof(plist));
		return 1;
	}
	mkdir(LD_DISABLE_DIR, 0755);
	fd = open(LD_DISABLE_PLIST, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, plist, strlen(plist)) != (ssize_t)strlen(plist)) {
		printf("init26: VAR %s: %s\n", LD_DISABLE_PLIST, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return 1;
	}
	close(fd);
	printf("init26: VAR ld_disable: %d services disabled for this boot (launchd's defaults kept)\n", count);
	return 0;
}

/*
 * MobileGestalt's cache goes when the loader presents another iPhone than the last boot did (identity-stamp.c).
 * Returns the number of failures.
 */
#define IDENTITY_STAMP          "/private/var/db/dlisa-identity"
#define GESTALT_CACHE           "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache" \
                                "/Library/Caches/com.apple.MobileGestalt.plist"

static int
resetGestaltWhenIdentityChanged(void)
{
	char machine[64], model[64], stamp[160], kept[160];
	size_t machineSize = sizeof(machine) - 1, modelSize = sizeof(model) - 1;
	int fd;
	ssize_t n;

	if (sysctlbyname("hw.machine", machine, &machineSize, NULL, 0) != 0 ||
	    sysctlbyname("hw.model", model, &modelSize, NULL, 0) != 0) {
		printf("init26: VAR identity: hw.machine/hw.model: %s\n", strerror(errno));
		return 1;
	}
	machine[machineSize] = model[modelSize] = 0;
	buildIdentityStamp(stamp, sizeof(stamp), machine, model);
	fd = open(IDENTITY_STAMP, O_RDONLY, 0);
	n = fd < 0 ? 0 : read(fd, kept, sizeof(kept));
	if (fd >= 0) {
		close(fd);
	}
	if (!isIdentityChanged(kept, n > 0 ? (size_t)n : 0, stamp)) {
		return 0;
	}
	if (unlink(GESTALT_CACHE) == 0) {
		printf("init26: VAR identity is now %s %s: MobileGestalt's cache removed\n", machine, model);
	}
	fd = open(IDENTITY_STAMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, stamp, strlen(stamp)) != (ssize_t)strlen(stamp)) {
		printf("init26: VAR %s: %s\n", IDENTITY_STAMP, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return 1;
	}
	close(fd);
	return 0;
}

/*
 * The data volume. On an iPhone /private/var is the APFS Data volume, created at
 * restore from the rootfs's private/var. Here, without var_ufs=1, /private/var is
 * ramfs (memory, empty at every boot) with the data volume's directory skeleton laid
 * down again (var-skel.h, generated from the IPSW). Without it launchd and the
 * daemons hit "Read-only file system" for /private/var/mobile/tmp, /var/run, and so
 * on. With the volume on the flash, the skeleton is laid down where it is missing
 * (the first boot of an empty volume: all of it) and what is there is left as the
 * boots before made it.
 */
static void
mountVar(void)
{
	char path[512];
	int made = 0, bad = 0, there = 0;
	int kept = mountFlashVar();
	struct stat st;

	if (!kept && mount("ramfs", "/private/var", 0, NULL) != 0) {
		printf("init26: VAR mount ramfs on /private/var: %s\n", strerror(errno));
		return;
	}
	if (kept) {
		static const char *const own[] = { "/private/var/tmp", "/private/var/run", "/private/var/vm" };

		for (unsigned i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
			emptyTree(own[i], 0);
		}
	}
	for (const struct var_skel *e = VAR_SKEL; e->path != NULL; e++) {
		size_t n = strlen("/private/var") + strlen(e->path) + 1;
		if (n > sizeof(path)) {
			continue;
		}
		strcpy(path, "/private/var");
		strcpy(path + strlen("/private/var"), e->path);
		if (kept && lstat(path, &st) == 0) {
			there++;
			continue;       /* made by an earlier boot: what Darwin made of it since is what counts */
		}
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
	/*
	 * /private/var/keybags is made by the restore on a real device (systembag.kb lives there) and is not in the
	 * rootfs listing the skeleton comes from. usermanagerd --init writes its user session and persona manifest
	 * there (usersession.kb, persona.kb); without the directory it cannot, and the daemon aborts at every start.
	 */
	if ((mkdir("/private/var/keybags", 0755) != 0 && !(kept && errno == 17 /* EEXIST */)) ||
	    chown("/private/var/keybags", 0, 0) != 0) {
		printf("init26: VAR /private/var/keybags: %s\n", strerror(errno));
		bad++;
	}
	bad += markMigrationDone();
	bad += saveDisabledServices();
	bad += resetGestaltWhenIdentityChanged();
	printf("init26: VAR /private/var is %s: %d entries made, %d already there, %d failed\n",
	    kept ? "the data volume on the flash" : "ramfs", made, there, bad);
	if (kept) {
		startVarSync();
	}
}

/*
 * The early boot tasks launchd skips here. launchd reads "rd=md0" in kern.bootargs and runs as a Restore
 * environment ("Restore environment starting."), and in it every boot task that is not PerformInRestore is
 * skipped -- `keybagd --init` and `usermanagerd --init` among them. Without the second, usermanagerd has no
 * persona manifest and aborts at every start ("Daemon failed to load persona manifest."), runningboardd waits
 * on it forever (UserManagement personaAttributesForPersonaType:), and backboardd and SpringBoard wait on
 * runningboardd: the window server never starts.
 *
 * Boot-arg init_run=PROG[:ARG...][;PROG[:ARG...]]...: run these, in order, before Apple's launchd, the way
 * launchd runs a boot task (XPC_NULL_BOOTSTRAP, XPC_SERVICES_UNAVAILABLE), each for init_run_secs seconds at
 * most (default 20; then it is killed). Nothing is run without the boot-arg. OS_ACTIVITY_DT_MODE makes their
 * os_log messages go to stderr too, which is the console here.
 */
static void
runOne(char *spec, unsigned secs)
{
	char *argv[8];
	int argc = 0;
	char *at = spec;
	char *envp[] = {
		"DYLD_SHARED_CACHE_DIR=/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld",
		"XPC_NULL_BOOTSTRAP=1", "XPC_SERVICES_UNAVAILABLE=1", "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
		"OS_ACTIVITY_DT_MODE=YES", NULL
	};
	int status = 0;
	pid_t pid, done = 0;

	while (at != NULL && *at != 0 && argc < 7) {
		char *colon = strchr(at, ':');

		argv[argc++] = at;
		if (colon == NULL) {
			break;
		}
		*colon = 0;
		at = colon + 1;
	}
	argv[argc] = NULL;
	if (argc == 0) {
		return;
	}
	pid = fork();
	if (pid == 0) {
		execve(argv[0], argv, envp);
		printf("init26: RUN cannot exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (pid < 0) {
		printf("init26: RUN fork for %s: %s\n", argv[0], strerror(errno));
		return;
	}
	printf("init26: RUN %s (pid %d, %d args)\n", argv[0], pid, argc);
	for (unsigned t = 0; t < secs * 10 && done == 0; t++) {
		done = wait4(pid, &status, 1 /* WNOHANG */, NULL);
		if (done == 0) {
			usleep(100000);
		}
	}
	if (done == 0) {
		mini_syscall(SYS_kill, pid, 9, 0, 0, 0, 0);
		wait4(pid, &status, 0, NULL);
		printf("init26: RUN %s: still running after %u s, killed\n", argv[0], secs);
		return;
	}
	printf("init26: RUN %s: exit status 0x%x\n", argv[0], status);
}

static char *
findArg(char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay != 0; hay++) {
		if (strncmp(hay, needle, n) == 0) {
			return hay;
		}
	}
	return NULL;
}

/* boot-arg console_open=1: anybody may write to /dev/console (diagnostic jobs that run as mobile) */
static int
findConsoleArg(void)
{
	static char args[1024];
	size_t len = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return 0;
	}
	args[len] = 0;
	return findArg(args, "console_open=1") != NULL;
}

static void
runEarlyTasks(void)
{
	static char args[1024];
	size_t len = sizeof(args) - 1;
	char *spec, *end;
	unsigned secs = 20;

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return;
	}
	args[len] = 0;
	spec = findArg(args, "init_run_secs=");
	if (spec != NULL) {
		secs = 0;
		for (spec += strlen("init_run_secs="); *spec >= '0' && *spec <= '9'; spec++) {
			secs = secs * 10 + (unsigned)(*spec - '0');
		}
	}
	spec = findArg(args, "init_run=");
	if (spec == NULL) {
		return;
	}
	spec += strlen("init_run=");
	end = strchr(spec, ' ');
	if (end != NULL) {
		*end = 0;
	}
	while (spec != NULL && *spec != 0) {
		char *semi = strchr(spec, ';');

		if (semi != NULL) {
			*semi = 0;
		}
		runOne(spec, secs);
		spec = semi ? semi + 1 : NULL;
	}
}

/*
 * /etc/dlisa-inject.conf: one "LIBRARY NAME NAME ..." per line. The kernel starts the programs with these file
 * names with the library inserted (sysctl kern.exec_inject, kernel/execaids; launchd will not pass DYLD_
 * variables from a job description). The library is libdlisa.dylib (userland/inject/).
 */
static void
configureInject(void)
{
	char conf[1024];
	int fd = open("/etc/dlisa-inject.conf", O_RDONLY, 0);
	ssize_t n;

	if (fd < 0) {
		return;
	}
	n = read(fd, conf, sizeof(conf) - 1);
	close(fd);
	while (n > 0 && (conf[n - 1] == '\n' || conf[n - 1] == ' ')) {
		n--;
	}
	if (n <= 0) {
		return;
	}
	conf[n] = 0;
	for (ssize_t i = 0; i < n; i++) {
		if (conf[i] == '\n') {
			conf[i] = ';';     /* the kernel separates the groups with ";" */
		}
	}
	if (sysctlbyname("kern.exec_inject", NULL, NULL, conf, (size_t)n + 1) != 0) {
		printf("init26: kern.exec_inject: %s\n", strerror(errno));
		return;
	}
	printf("init26: library insertion: %s\n", conf);
}

#include "rtc-wifi-boot.c"

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
	/* darwin-lisa: the clock's kept offset (rtc=1) and the kept Wi-Fi network (wifi=1), init/rtc-wifi-boot.c */
	applySavedRtcOffset();
	startRtcKeeper();
	giveWifiNetwork();
	/* darwin-lisa: diagnostics jobs that run as mobile (com.darwinlisa.*) write to the console, which is the log */
	if (findConsoleArg()) {
		chmod("/dev/console", 0666);
	}
	configureInject();
	runEarlyTasks();

	handOff(envp);

	printf("init26: first-boot checks done, halting.\n");
	reboot(0x08 /* RB_HALT */);
	return 0;
}
