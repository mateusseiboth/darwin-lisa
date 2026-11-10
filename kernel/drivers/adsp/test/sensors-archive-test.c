/*
 * sensors-archive-test ARCHIVE ROOT: the archive fw-sensors-stage.py packed, read back through the kernel's own file
 * server (dlisa.fastrpc_proto.h), against the staged tree ROOT on the host's disk: every file opened and read whole
 * through apps_std, every directory listed through opendir/readdir. Exit status 0 when all agree.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../dlisa.fastrpc_proto.h"

static int failures, files, directories;

static void *a(void *ctx, uint32_t size) { (void)ctx; return calloc(1, size ? size : 1); }
static void f(void *ctx, void *p, uint32_t size) { (void)ctx; (void)size; free(p); }
static const struct frpc_ops ops = { a, f, NULL, NULL, NULL };

static uint8_t *
slurp(const char *path, uint32_t *size)
{
	FILE *file = fopen(path, "rb");
	uint8_t *data;
	long n;

	if (file == NULL) {
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	n = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)n + 1);
	if (fread(data, 1, (size_t)n, file) != (size_t)n) {
		n = 0;
	}
	fclose(file);
	*size = (uint32_t)n;
	return data;
}

static uint32_t
call(struct frpc_server *s, uint32_t method, uint8_t *prim, uint32_t prim_size, const char *string, uint32_t outs,
    uint32_t *out_size, uint8_t (*out)[512])
{
	struct frpc_view in[2] = { { prim, prim_size }, { (const uint8_t *)string, string ? (uint32_t)strlen(string) + 1 : 0 } };
	struct frpc_buf ob[2];

	for (uint32_t i = 0; i < outs; i++) {
		ob[i].data = out[i];
		ob[i].length = out_size[i];
	}
	return frpc_server_call(s, FRPC_HANDLE_APPS_STD, FRPC_SC(method, string ? 2 : 1, outs), in, ob);
}

static void
check_file(struct frpc_server *s, const char *disk, const char *name)
{
	uint8_t prim[8], out[2][512], *expected, *got;
	uint32_t sizes[2], size, total = 0, fd;

	expected = slurp(disk, &size);
	got = malloc(size + 512);
	frpc_wr32(prim, (uint32_t)strlen(name) + 1);
	frpc_wr32(prim + 4, 2);
	{
		struct frpc_view in[3] = { { prim, 8 }, { (const uint8_t *)name, (uint32_t)strlen(name) + 1 }, { (const uint8_t *)"r", 2 } };
		struct frpc_buf ob[1] = { { out[0], 4 } };

		if (frpc_server_call(s, FRPC_HANDLE_APPS_STD, FRPC_SC(0, 3, 1), in, ob) != 0) {
			printf("FAIL: fopen %s\n", name);
			failures++;
			goto out;
		}
	}
	fd = frpc_rd32(out[0]);
	for (;;) {
		frpc_wr32(prim, fd);
		frpc_wr32(prim + 4, 512);
		sizes[0] = 8;
		sizes[1] = 512;
		if (call(s, 4, prim, 8, NULL, 2, sizes, out) != 0) {
			printf("FAIL: fread %s\n", name);
			failures++;
			break;
		}
		memcpy(got + total, out[1], frpc_rd32(out[0]));
		total += frpc_rd32(out[0]);
		if (frpc_rd32(out[0] + 4) || total > size) {
			break;
		}
	}
	if (total != size || memcmp(got, expected, size) != 0) {
		printf("FAIL: %s: %u bytes through the server, %u on disk, or different\n", name, total, size);
		failures++;
	}
	frpc_wr32(prim, fd);
	call(s, 3, prim, 4, NULL, 0, sizes, out);
	files++;
out:
	free(got);
	free(expected);
}

static int
compare(const void *x, const void *y)
{
	return strcmp(*(char *const *)x, *(char *const *)y);
}

/* the server's names and the disk's, sorted, as one '|'-separated string each */
static void
check_directory(struct frpc_server *s, const char *disk, const char *name)
{
	char *mine[1024], *theirs[1024], joined[2][65536];
	uint32_t n_mine = 0, n_theirs = 0, sizes[2] = { 8, 264 };
	uint8_t prim[8], out[2][512];
	uint64_t dir;
	DIR *d = opendir(disk);
	struct dirent *e;

	while (d != NULL && (e = readdir(d)) != NULL && n_theirs < 1024) {
		theirs[n_theirs++] = strdup(e->d_name);
	}
	closedir(d);
	frpc_wr32(prim, (uint32_t)strlen(name) + 1);
	if (call(s, 26, prim, 4, name, 1, sizes, out) != 0) {
		printf("FAIL: opendir %s\n", name);
		failures++;
		return;
	}
	dir = frpc_rd64(out[0]);
	for (;;) {
		frpc_wr64(prim, dir);
		sizes[0] = 264;
		if (call(s, 28, prim, 8, NULL, 1, sizes, out) != 0 || frpc_rd32(out[0] + 260) || n_mine == 1024) {
			break;
		}
		mine[n_mine++] = strdup((char *)out[0] + 4);
	}
	frpc_wr64(prim, dir);
	call(s, 27, prim, 8, NULL, 0, sizes, out);
	qsort(mine, n_mine, sizeof(char *), compare);
	qsort(theirs, n_theirs, sizeof(char *), compare);
	joined[0][0] = joined[1][0] = 0;
	for (uint32_t i = 0; i < n_mine; i++) {
		strcat(strcat(joined[0], mine[i]), "|");
		free(mine[i]);
	}
	for (uint32_t i = 0; i < n_theirs; i++) {
		strcat(strcat(joined[1], theirs[i]), "|");
		free(theirs[i]);
	}
	if (strcmp(joined[0], joined[1]) != 0) {
		printf("FAIL: %s lists\n  %s\n  on disk\n  %s\n", name, joined[0], joined[1]);
		failures++;
	}
	directories++;
}

static void
walk(struct frpc_server *s, const char *root, const char *name)
{
	char disk[4096], child[512];
	DIR *d;
	struct dirent *e;

	snprintf(disk, sizeof(disk), "%s%s", root, name);
	check_directory(s, disk, name);
	d = opendir(disk);
	while ((e = readdir(d)) != NULL) {
		struct stat st;
		char path[4096];

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		snprintf(child, sizeof(child), "%s/%s", strcmp(name, "/") == 0 ? "" : name, e->d_name);
		snprintf(path, sizeof(path), "%s%s", root, child);
		stat(path, &st);
		if (S_ISDIR(st.st_mode)) {
			walk(s, root, child);
		} else {
			check_file(s, path, child);
		}
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	struct frpc_server *s = calloc(1, sizeof(*s));
	uint32_t size;
	uint8_t *archive;

	if (argc != 3 || (archive = slurp(argv[1], &size)) == NULL) {
		fprintf(stderr, "usage: sensors-archive-test ARCHIVE ROOT\n");
		return 2;
	}
	if (!frpc_server_init(s, &ops, archive, size)) {
		printf("FAIL: the server refuses the archive\n");
		return 1;
	}
	walk(s, argv[2], "/");
	printf("sensors-archive-test: %u files and %u directories of %s through the server, %d failures\n", files,
	    directories, argv[2], failures);
	free(archive);
	free(s);
	return failures != 0;
}
