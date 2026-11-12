/*
 * dirtest DIR: how the kernel enumerates a directory for the three readers iOS uses, printed to stdout (the console
 * when run as init_run=/usr/local/bin/dirtest:/Applications): readdir(3), getattrlistbulk(2) as Foundation's
 * NSFileManager asks (name, object type, error) and stat(2) of every entry. Made for the 25-of-252 applications
 * MobileInstallation's scanner found in /Applications on the APFS port (2026-11-11): says whether the kernel or the
 * scanner left the rest out.
 *
 * Build (darwin-lisa/userland/tools/build-dirtest.sh): the iOS SDK, plain arm64, ad-hoc signed; goes to the sysroot's
 * /usr/local/bin.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/stat.h>

#define VDIR 2                  /* fsobj_type_t: enum vtype of the kernel's sys/vnode.h, which the SDK does not ship */
#include <unistd.h>

/* the attributes come back in bit order: RETURNED_ATTRS, NAME, OBJTYPE, then ERROR (0x20000000) last */
struct bulk_entry {
	uint32_t length;
	attribute_set_t returned;
	attrreference_t name;
	fsobj_type_t type;
	uint32_t error;
};

static unsigned
countReaddir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	unsigned n = 0, apps = 0;

	if (d == NULL) {
		printf("dirtest: opendir %s: %s\n", dir, strerror(errno));
		return 0;
	}
	while ((de = readdir(d)) != NULL) {
		n++;
		apps += strlen(de->d_name) > 4 && strcmp(de->d_name + strlen(de->d_name) - 4, ".app") == 0;
	}
	closedir(d);
	printf("dirtest: readdir: %u entries, %u .app\n", n, apps);
	return n;
}

static unsigned
countBulk(const char *dir)
{
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
		             .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE };
	char buf[32 * 1024];
	unsigned total = 0, calls = 0, dirs = 0, errs = 0;
	int fd = open(dir, O_RDONLY | O_DIRECTORY);

	if (fd < 0) {
		printf("dirtest: open %s: %s\n", dir, strerror(errno));
		return 0;
	}
	for (;;) {
		int n = getattrlistbulk(fd, &al, buf, sizeof(buf), 0);
		char *at = buf;

		calls++;
		if (n < 0) {
			printf("dirtest: getattrlistbulk call %u: %s\n", calls, strerror(errno));
			break;
		}
		if (n == 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			struct bulk_entry *e = (struct bulk_entry *)at;
			const char *name = (const char *)&e->name + e->name.attr_dataoffset;
			int has_error = (e->returned.commonattr & ATTR_CMN_ERROR) != 0;
			int has_type = (e->returned.commonattr & ATTR_CMN_OBJTYPE) != 0;

			total++;
			if (has_error && e->error != 0) {
				errs++;
				printf("dirtest: bulk entry %s: error %u (%s)\n", name, e->error, strerror((int)e->error));
			} else if (!has_type) {
				errs++;
				printf("dirtest: bulk entry %s: no object type returned (attrs %#x)\n", name, e->returned.commonattr);
			} else if (e->type == VDIR) {
				dirs++;
			}
			at += e->length;
		}
		printf("dirtest: getattrlistbulk call %u: %d entries\n", calls, n);
	}
	close(fd);
	printf("dirtest: getattrlistbulk: %u entries in %u calls, %u directories, %u with an error\n", total, calls, dirs, errs);
	return total;
}

static void
statAll(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	unsigned ok = 0, bad = 0;

	if (d == NULL) {
		return;
	}
	while ((de = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;

		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) == 0) {
			ok++;
		} else {
			bad++;
			printf("dirtest: stat %s: %s\n", path, strerror(errno));
		}
	}
	closedir(d);
	printf("dirtest: stat: %u ok, %u failed\n", ok, bad);
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/Applications";

	printf("dirtest: %s\n", dir);
	countReaddir(dir);
	countBulk(dir);
	statAll(dir);
	return 0;
}
