/*
 * Checks pac_rewrite() against the vectors of tools/pacstrip/vectors.py,
 * which come from the Python implementation that the exhaustive tests check
 * against llvm-mc.
 *
 * usage: test_pac_rewrite VECTORS
 */
#include <stdio.h>

#include "pac_rewrite.h"

/* retab, nop, braa x16, x17 -> ret, nop, br x16: two patched */
static unsigned long check_buffer(void)
{
	uint32_t words[] = { 0xD65F0FFF, 0xD503201F, 0xD71F0A11 };
	const uint32_t want[] = { 0xD65F03C0, 0xD503201F, 0xD61F0200 };
	size_t patched = pac_rewrite_buffer(words, 3);
	unsigned long failures = patched != 2;
	size_t i;

	for (i = 0; i < 3; i++)
		failures += words[i] != want[i];
	if (failures)
		fprintf(stderr, "pac_rewrite_buffer: %zu patched, %08x %08x %08x\n",
			patched, words[0], words[1], words[2]);
	return failures;
}

int main(int argc, char **argv)
{
	unsigned int word, status, expected;
	unsigned long count = 0, failures = 0;
	FILE *vectors;

	if (argc != 2) {
		fprintf(stderr, "usage: %s VECTORS\n", argv[0]);
		return 2;
	}
	vectors = fopen(argv[1], "r");
	if (!vectors) {
		perror(argv[1]);
		return 2;
	}
	while (fscanf(vectors, "%x %u %x", &word, &status, &expected) == 3) {
		uint32_t result = word;
		enum pac_rewrite_status got = pac_rewrite(&result);

		count++;
		if ((unsigned int)got == status && result == expected)
			continue;
		if (failures++ < 20)
			fprintf(stderr, "%08x: got %d %08x, want %u %08x\n", word, got, result, status, expected);
	}
	fclose(vectors);
	failures += check_buffer();
	printf("%lu vectors, %lu failures\n", count, failures);
	return count == 0 || failures != 0;
}
