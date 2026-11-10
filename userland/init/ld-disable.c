/*
 * init26: boot-arg ld_disable=TOKEN,TOKEN,... -> launchd's disabled-services file for the system domain,
 * /private/var/db/com.apple.xpc.launchd/disabled.plist (launchd 26: "%s/disabled.plist", label -> true = disabled).
 *
 * It lets a boot leave LaunchDaemons out without a new root image, which is how the full image is bisected against the
 * lean one (userland/scripts/lean-daemons.txt). A TOKEN is
 *   - a group of /etc/dlisa-ld-groups.conf ("NAME label label ...", made from lean-daemons.txt), or
 *   - a full label (com.apple.locationd), or
 *   - a short one (locationd = com.apple.locationd).
 * The file is written at every boot, with launchd's own defaults (what launchd writes into an empty one), the
 * groups file's "default" group (the services this port always leaves off; ld_default=0 leaves them on) and the
 * boot-arg's labels: a list never outlives the boot that asked for it.
 *
 * Pure string work, no system calls: init-26.c includes it after mini.h, test/ld-disable-test.c after <string.h>.
 */

#define LD_DISABLE_MAX_LABELS   160
#define LD_DISABLE_LABEL_SIZE   96

struct ldLabel {
	char name[LD_DISABLE_LABEL_SIZE];
	int disabled;
	int ours;               /* asked for by the boot-arg (else one of launchd's defaults) */
};

struct ldLabels {
	struct ldLabel item[LD_DISABLE_MAX_LABELS];
	unsigned count;
};

/* what launchd 26 writes into a data volume that has no disabled.plist yet (read back from the phone's volume) */
static const struct {
	const char *name;
	int disabled;
} LD_DISABLE_DEFAULTS[] = {
	{ "com.apple.dhcp6d", 1 },
	{ "com.apple.bootpd", 1 },
	{ "com.apple.ftp-proxy-embedded", 0 },
};

static int
isLdSeparator(char c)
{
	return c == 0 || c == ' ' || c == ',' || c == '\n' || c == '\t';
}

static unsigned
measureLdWord(const char *at)
{
	unsigned n = 0;

	while (!isLdSeparator(at[n])) {
		n++;
	}
	return n;
}

/* the label (length bytes at text, com.apple. put in front of a short one) as it goes in the file */
static int
makeLdLabel(char *out, const char *text, unsigned length)
{
	static const char prefix[] = "com.apple.";
	int isShort = 1;
	unsigned at = 0;

	for (unsigned i = 0; i < length; i++) {
		isShort &= text[i] != '.';
	}
	if (length == 0 || length + sizeof(prefix) > LD_DISABLE_LABEL_SIZE) {
		return 0;
	}
	if (isShort) {
		memcpy(out, prefix, sizeof(prefix) - 1);
		at = sizeof(prefix) - 1;
	}
	memcpy(out + at, text, length);
	out[at + length] = 0;
	return 1;
}

static void
setLdLabel(struct ldLabels *labels, const char *name, int disabled, int ours)
{
	for (unsigned i = 0; i < labels->count; i++) {
		if (strcmp(labels->item[i].name, name) == 0) {
			labels->item[i].disabled = disabled;
			labels->item[i].ours |= ours;
			return;
		}
	}
	if (labels->count == LD_DISABLE_MAX_LABELS) {
		return;
	}
	strcpy(labels->item[labels->count].name, name);
	labels->item[labels->count].disabled = disabled;
	labels->item[labels->count].ours = ours;
	labels->count++;
}

/* the group NAME's labels from the groups file, or NULL when there is no such group */
static const char *
findLdGroup(const char *groups, const char *name, unsigned length)
{
	for (const char *line = groups; line != NULL && *line != 0; line = strchr(line, '\n'), line += line != NULL) {
		if (*line != '#' && strncmp(line, name, length) == 0 && (line[length] == ' ' || line[length] == '\t')) {
			return line + length;
		}
	}
	return NULL;
}

static void
addLdGroup(struct ldLabels *labels, const char *members)
{
	char label[LD_DISABLE_LABEL_SIZE];

	while (*members != 0 && *members != '\n') {
		unsigned n = measureLdWord(members);

		if (n > 0 && makeLdLabel(label, members, n)) {
			setLdLabel(labels, label, 1, 1);
		}
		members += n + (members[n] == ' ' || members[n] == '\t' || members[n] == ',');
	}
}

/* the value of boot-arg NAME= (name with its "=", a whole word of the boot-args), or NULL */
static const char *
findLdArg(const char *bootArgs, const char *name)
{
	size_t n = strlen(name);

	for (const char *p = bootArgs; *p != 0; p++) {
		if ((p == bootArgs || p[-1] == ' ') && strncmp(p, name, n) == 0) {
			return p + n;
		}
	}
	return NULL;
}

static int
isLdDefaultWanted(const char *bootArgs)
{
	const char *value = findLdArg(bootArgs, "ld_default=");

	return value == NULL || *value != '0';
}

static void
addLdTokens(struct ldLabels *labels, const char *value, const char *groups)
{
	char label[LD_DISABLE_LABEL_SIZE];

	while (value != NULL && *value != 0 && *value != ' ') {
		unsigned n = measureLdWord(value);
		const char *members = groups != NULL && n > 0 ? findLdGroup(groups, value, n) : NULL;

		if (members != NULL) {
			addLdGroup(labels, members);
		} else if (n > 0 && makeLdLabel(label, value, n)) {
			setLdLabel(labels, label, 1, 1);
		}
		value += n + (value[n] == ',');
	}
}

struct ldWriter {
	char *out;
	size_t size, used;
	int full;
};

static void
appendLd(struct ldWriter *w, const char *text)
{
	size_t n = strlen(text);

	if (w->full || w->used + n + 1 > w->size) {
		w->full = 1;
		return;
	}
	memcpy(w->out + w->used, text, n);
	w->used += n;
	w->out[w->used] = 0;
}

/*
 * The disabled.plist for these boot-args into out (size bytes, NUL-terminated). groups: the groups file's text, or
 * NULL. Returns how many labels the boot-arg disabled, or -1 when out is too small.
 */
static int
buildDisabledPlist(const char *bootArgs, const char *groups, char *out, size_t size)
{
	static struct ldLabels labels;
	struct ldWriter w = { out, size, 0, 0 };
	const char *defaults;
	int ours = 0;

	labels.count = 0;
	for (unsigned i = 0; i < sizeof(LD_DISABLE_DEFAULTS) / sizeof(LD_DISABLE_DEFAULTS[0]); i++) {
		setLdLabel(&labels, LD_DISABLE_DEFAULTS[i].name, LD_DISABLE_DEFAULTS[i].disabled, 0);
	}
	defaults = groups != NULL && isLdDefaultWanted(bootArgs) ? findLdGroup(groups, "default", 7) : NULL;
	if (defaults != NULL) {
		addLdGroup(&labels, defaults);
	}
	addLdTokens(&labels, findLdArg(bootArgs, "ld_disable="), groups);
	appendLd(&w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n<dict>\n");
	for (unsigned i = 0; i < labels.count; i++) {
		appendLd(&w, "\t<key>");
		appendLd(&w, labels.item[i].name);
		appendLd(&w, labels.item[i].disabled ? "</key>\n\t<true/>\n" : "</key>\n\t<false/>\n");
		ours += labels.item[i].ours;
	}
	appendLd(&w, "</dict>\n</plist>\n");
	return w.full ? -1 : ours;
}
