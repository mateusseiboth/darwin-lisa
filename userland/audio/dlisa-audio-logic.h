/*
 * darwin-lisa: the pure part of DLisaAudio.driver (dlisa-audio.c): which devices it publishes, the volume control's
 * scale, the device clock. No CoreFoundation, no system calls: test/dlisa_audio_logic_test.c runs it on the host.
 *
 * Boot-args (kern.bootargs):
 *   dlisa_audio=0               the plug-in loads and publishes nothing (the system as without it)
 *   dlisa_audio_devs=LIST       which devices, comma-separated roles, each optionally ROLE:UID (the UID the virtual
 *                               audio device knows it by). Roles: speaker, receiver, hawking, actuator, codec.
 *                               Default: speaker,hawking,actuator,codec with UIDs Speaker, Hawking, Actuator, Codec
 *                               (HakiOS's set, which VirtualAudio routed on iOS 16; the names iOS 26's
 *                               BuiltinAudioPlugin also gives an iPhone's speaker and receiver).
 */
#ifndef DLISA_AUDIO_LOGIC_H
#define DLISA_AUDIO_LOGIC_H

enum { DLISA_SPEAKER, DLISA_RECEIVER, DLISA_HAWKING, DLISA_ACTUATOR, DLISA_CODEC, DLISA_ROLES };

#define DLISA_ROLE_SPEAKER      (1u << DLISA_SPEAKER)
#define DLISA_ROLE_RECEIVER     (1u << DLISA_RECEIVER)
#define DLISA_ROLE_HAWKING      (1u << DLISA_HAWKING)
#define DLISA_ROLE_ACTUATOR     (1u << DLISA_ACTUATOR)
#define DLISA_ROLE_CODEC        (1u << DLISA_CODEC)
#define DLISA_ROLES_DEFAULT     (DLISA_ROLE_SPEAKER | DLISA_ROLE_HAWKING | DLISA_ROLE_ACTUATOR | DLISA_ROLE_CODEC)

/* the volume control's scale: a straight line of decibels, 0 dB at the top (HakiOS's: 3 dB for each of 16 steps) */
#define DLISA_AUDIO_VOLUME_RANGE_DB 48.0f

static const char *const dlisa_role_names[DLISA_ROLES] = { "speaker", "receiver", "hawking", "actuator", "codec" };
static const char *const dlisa_role_uids[DLISA_ROLES] = { "Speaker", "Receiver", "Hawking", "Actuator", "Codec" };

struct dlisa_audio_choice {
	bool on;
	unsigned roles;                 /* DLISA_ROLE_* */
	char uid[DLISA_ROLES][32];
};

/* the value of KEY=... in a boot-args line, as a pointer into it and a length; NULL when absent */
static inline const char *
dlisa_audio_arg(const char *line, const char *key, size_t *length)
{
	size_t k = strlen(key);

	for (const char *at = line; at != NULL && *at != 0; at = strchr(at, ' ')) {
		while (*at == ' ') {
			at++;
		}
		if (strncmp(at, key, k) == 0 && at[k] == '=') {
			const char *value = at + k + 1;
			const char *end = strchr(value, ' ');

			*length = end != NULL ? (size_t)(end - value) : strlen(value);
			return value;
		}
	}
	return NULL;
}

static inline int
dlisa_audio_role_of(const char *name, size_t length)
{
	for (int r = 0; r < DLISA_ROLES; r++) {
		if (strlen(dlisa_role_names[r]) == length && strncmp(dlisa_role_names[r], name, length) == 0) {
			return r;
		}
	}
	return -1;
}

static inline void
dlisa_audio_copy(char *out, size_t size, const char *from, size_t length)
{
	length = length < size - 1 ? length : size - 1;
	memcpy(out, from, length);
	out[length] = 0;
}

/* one item of dlisa_audio_devs: ROLE or ROLE:UID */
static inline void
dlisa_audio_take(struct dlisa_audio_choice *c, const char *item, size_t length)
{
	const char *colon = memchr(item, ':', length);
	size_t name_length = colon != NULL ? (size_t)(colon - item) : length;
	int role = dlisa_audio_role_of(item, name_length);

	if (role < 0) {
		return;
	}
	c->roles |= 1u << role;
	if (colon != NULL && length > name_length + 1) {
		dlisa_audio_copy(c->uid[role], sizeof(c->uid[role]), colon + 1, length - name_length - 1);
	}
}

static inline void
dlisa_audio_choose(struct dlisa_audio_choice *c, const char *bootargs)
{
	size_t length = 0;
	const char *value = dlisa_audio_arg(bootargs, "dlisa_audio", &length);

	c->on = !(value != NULL && length == 1 && value[0] == '0');
	c->roles = DLISA_ROLES_DEFAULT;
	for (int r = 0; r < DLISA_ROLES; r++) {
		dlisa_audio_copy(c->uid[r], sizeof(c->uid[r]), dlisa_role_uids[r], strlen(dlisa_role_uids[r]));
	}
	value = dlisa_audio_arg(bootargs, "dlisa_audio_devs", &length);
	if (value == NULL) {
		return;
	}
	c->roles = 0;
	for (const char *end = value + length; value < end;) {
		const char *comma = memchr(value, ',', (size_t)(end - value));
		size_t item = comma != NULL ? (size_t)(comma - value) : (size_t)(end - value);

		dlisa_audio_take(c, value, item);
		value += item + (comma != NULL);
	}
}

static inline float
dlisa_audio_scalar_to_db(float scalar)
{
	scalar = scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
	return -DLISA_AUDIO_VOLUME_RANGE_DB * (1.0f - scalar);
}

static inline float
dlisa_audio_db_to_scalar(float db)
{
	float scalar = 1.0f + db / DLISA_AUDIO_VOLUME_RANGE_DB;

	return scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
}

/* what HAKIOS_AUDIO_SET_VOLUME takes: eighths of a dB under the loudest the kernel allows, and muted */
struct dlisa_audio_kernel_volume {
	uint32_t below, muted;
};

static inline struct dlisa_audio_kernel_volume
dlisa_audio_kernel_volume(float scalar)
{
	float below = -dlisa_audio_scalar_to_db(scalar) * 8.0f;

	return (struct dlisa_audio_kernel_volume){ (uint32_t)(below + 0.5f), scalar <= 0 };
}

/* the device's clock: whole periods of `period' frames since `anchor' (mach_absolute_time() when I/O started) */
struct dlisa_audio_clock {
	uint64_t anchor, periods;
};

static inline void
dlisa_audio_zero_time(struct dlisa_audio_clock *clk, uint64_t now, double ticks_per_frame, uint32_t period, double *sample,
    uint64_t *host)
{
	double ticks_per_period = ticks_per_frame * period;

	while (now >= clk->anchor + (uint64_t)((double)(clk->periods + 1) * ticks_per_period)) {
		clk->periods++;
	}
	*sample = (double)clk->periods * period;
	*host = clk->anchor + (uint64_t)((double)clk->periods * ticks_per_period);
}

#endif /* DLISA_AUDIO_LOGIC_H */
