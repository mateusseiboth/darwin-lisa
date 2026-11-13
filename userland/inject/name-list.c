/*
 * libdlisa: is this program named by a boot-arg that lists programs (dlisa_oslog=NAME,NAME,... or =all or =1,
 * dlisa_uiprobe=...)? `line` is kern.bootargs as it is.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* the value of boot-arg `key` in `line` (what follows "key="), or NULL */
static const char *
findBootArgValue(const char *line, const char *key)
{
	size_t length = strlen(key);
	const char *at = line;

	while (at != NULL && (at = strstr(at, key)) != NULL) {
		if ((at == line || at[-1] == ' ') && at[length] == '=') {
			return at + length + 1;
		}
		at += length;
	}
	return NULL;
}

static bool
isNamedByBootArg(const char *line, const char *key, const char *name)
{
	const char *at = line != NULL ? findBootArgValue(line, key) : NULL;
	size_t length = strlen(name);

	if (at == NULL) {
		return false;
	}
	if (strncmp(at, "all", 3) == 0 || (at[0] == '1' && (at[1] == ' ' || at[1] == 0))) {
		return true;
	}
	while (*at != 0 && *at != ' ') {
		if (strncmp(at, name, length) == 0 && (at[length] == ',' || at[length] == ' ' || at[length] == 0)) {
			return true;
		}
		at += strcspn(at, ", ");
		at += *at == ',';
	}
	return false;
}
