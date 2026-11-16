/*
 * libdlisa: boot-arg dlisa_sndtest=SECS[:SOUND],... (SpringBoard): at SECS seconds after SpringBoard started, play
 * /System/Library/Audio/UISounds/SOUND.caf (default ReceivedMessage) as a system sound, the way any app's
 * AudioServicesPlaySystemSound reaches audiomxd. It tests the whole way to the speakers with nobody at the phone: the
 * plug-in's and the kernel's lines say what came out. A name is letters, digits, '_' and '-' only.
 */
#ifndef DLISA_SOUND_TEST_C
#define DLISA_SOUND_TEST_C
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOUND_TEST_STEPS        8
#define SOUND_TEST_DEFAULT      "ReceivedMessage"

struct soundTestStep {
	unsigned seconds;
	char sound[40];
};

static bool
isSoundNameChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* one SECS[:SOUND] item of `length' characters */
static bool
parseSoundTestStep(const char *item, size_t length, struct soundTestStep *step)
{
	const char *colon = memchr(item, ':', length);
	size_t name = colon != NULL ? length - (size_t)(colon - item) - 1 : 0;
	bool good = name > 0 && name < sizeof(step->sound);

	if (length == 0 || item[0] < '0' || item[0] > '9') {
		return false;
	}
	step->seconds = (unsigned)strtoul(item, NULL, 10);
	for (size_t i = 0; good && i < name; i++) {
		good = isSoundNameChar(colon[1 + i]);
	}
	if (good) {
		memcpy(step->sound, colon + 1, name);
		step->sound[name] = 0;
	} else {
		strcpy(step->sound, SOUND_TEST_DEFAULT);
	}
	return true;
}

static unsigned
parseSoundTest(const char *bootargs, struct soundTestStep steps[SOUND_TEST_STEPS])
{
	const char *key = "dlisa_sndtest=", *at = bootargs;
	unsigned n = 0;

	while (at != NULL && (at = strstr(at, key)) != NULL && at != bootargs && at[-1] != ' ') {
		at += strlen(key);
	}
	if (at == NULL) {
		return 0;
	}
	at += strlen(key);
	while (n < SOUND_TEST_STEPS && *at != 0 && *at != ' ') {
		size_t length = strcspn(at, ", ");

		n += parseSoundTestStep(at, length, &steps[n]);
		at += length + (at[length] == ',');
	}
	return n;
}

static const char *
soundTestPath(const char *sound, char *out, size_t size)
{
	snprintf(out, size, "/System/Library/Audio/UISounds/%s.caf", sound);
	return out;
}
#endif
