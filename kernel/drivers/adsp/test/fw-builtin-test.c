/*
 * Host test of `fw-adsp-stage.py header`: the generated hakios.adsp_fw.h, compiled for this machine, must give back
 * every staged file byte for byte, page-aligned, with the header file first (fw-builtin-test.sh runs it).
 * Usage: fw-builtin-test DIR    (the staged directory the header was made from)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hakios.adsp_fw.h"

static int
matchesFile(const char *dir, const char *name, const uint8_t *data, uint64_t size)
{
	char path[512];
	FILE *f;
	uint8_t *want;
	long length;
	int same;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL) {
		return 0;
	}
	fseek(f, 0, SEEK_END);
	length = ftell(f);
	fseek(f, 0, SEEK_SET);
	want = malloc((size_t)length);
	same = want != NULL && fread(want, 1, (size_t)length, f) == (size_t)length && (uint64_t)length == size &&
	    memcmp(want, data, size) == 0;
	free(want);
	fclose(f);
	return same;
}

int
main(int argc, char **argv)
{
	unsigned count = sizeof(adsp_builtin_fw) / sizeof(adsp_builtin_fw[0]), failures = 0;
	uint64_t total = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: fw-builtin-test DIR\n");
		return 2;
	}
	if (count < 4 || strcmp(adsp_builtin_fw[0].name, "adsp.mdt") != 0) {
		printf("FAIL: %u entries, the first \"%s\" (wanted adsp.mdt first)\n", count, count ? adsp_builtin_fw[0].name : "");
		failures++;
	}
	for (unsigned i = 0; i < count; i++) {
		if (((uintptr_t)adsp_builtin_fw[i].data & 0xfff) != 0) {
			printf("FAIL: %s is not on a 4 KB boundary\n", adsp_builtin_fw[i].name);
			failures++;
		}
		if (!matchesFile(argv[1], adsp_builtin_fw[i].name, adsp_builtin_fw[i].data, adsp_builtin_fw[i].size)) {
			printf("FAIL: %s differs from %s/%s\n", adsp_builtin_fw[i].name, argv[1], adsp_builtin_fw[i].name);
			failures++;
		}
		total += adsp_builtin_fw[i].size;
	}
	if (failures != 0) {
		printf("fw-builtin-test: %u FAILED\n", failures);
		return 1;
	}
	printf("fw-builtin-test: %u files, %llu bytes, all equal to %s\n", count, (unsigned long long)total, argv[1]);
	return 0;
}
