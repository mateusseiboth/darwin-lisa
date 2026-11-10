/*
 * darwin-lisa: the sensors of lisa (Xiaomi 11 Lite 5G NE, SM7325) through the audio DSP's Snapdragon Sensor Core.
 *
 * On SM7325 the accelerometer/gyroscope (TDK ICM-4x6xx), the magnetometer (AKM AK0991x), the light and proximity
 * sensor (Sensortek STK3A6x) and the SAR sensors sit on the ADSP's sensor island, not on an I2C bus of ours. The
 * DSP's sensors framework is reached as a QMI service on the DSP's QRTR (hakios.adsp.c, adsp=1): service 400,
 * "SNS client" (libqmi's SSC service). Its single request, 0x20, carries a protobuf message (Qualcomm's
 * sns_client.proto; the field numbers here were read from pmOS's libssc.so.2 protobuf-c descriptors:
 * kernel/drivers/adsp/README.md), and the events come back as indications 0x21 (small) and 0x22 (large), each a
 * protobuf too. This file is that client, in the kernel:
 *   1. wait for service 400 among the services the DSP tells of (up to sensors_wait seconds, default 90);
 *   2. ask the "SUID lookup" sensor for the SUIDs of accel, gyro, mag, ambient_light, proximity (and registry, for
 *      the log: without it the physical sensors have no configuration, see below);
 *   3. enable what was found (accel/gyro/mag at sensors_rate Hz, default 10; light/proximity on change);
 *   4. keep the last values in the sysctl kern.hakios.sensors, which userland/motion's backboardd hook reads
 *      (the screen's rotation), and log a line every 10 s.
 *
 * FEASIBILITY, honestly: on Linux the same service works only with hexagonrpcd running (libssc: "'registry' sensor
 * unavailable, is hexagonrpcd running?"). The DSP's sensors PD reads its registry and the drivers' JSON (pmOS:
 * /usr/share/qcom/sensors) from the host through FastRPC's file service, and pmOS's lisa image does NOT ship
 * hexagonrpcd: sensors do not work in pmOS on lisa either. So this client may well find service 400 with only the
 * framework's virtual sensors, or not at all. What it prints says which; the FastRPC file server is the next step
 * (README.md, "Sensors: what is missing").
 *
 * sensors_fake=2 does the same only when the real accelerometer has given nothing (fallback_thread()).
 * sensors_fake=1 needs no DSP at all: a gravity vector that turns a quarter every 15 s (portrait, landscape with the
 * bottom to the right, upside down, landscape with the bottom to the left), for testing the iOS side (rotation) on
 * its own. It is started from hakios_adsp_init(), before adsp=1 is looked at.
 *
 * No floating point in the kernel: the values are kept as IEEE-754 single-precision bits (the protobuf's own
 * encoding), and userland reads them as floats. Rotation (sensors_rot=90|180|270) and sign changes work on the bits.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

/* hakios.adsp.c (same tag) */
typedef void (*dlisa_adsp_receive_t)(uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
extern bool dlisa_adsp_up(void);
extern bool dlisa_adsp_qrtr_service(uint32_t service, uint32_t *node, uint32_t *port, uint32_t *instance);
extern uint32_t dlisa_adsp_qrtr_open(dlisa_adsp_receive_t receive);
extern void dlisa_adsp_qrtr_close(uint32_t number);
extern int dlisa_adsp_qrtr_send(uint32_t from_port, uint32_t node, uint32_t port, const void *data, uint32_t length);

void dlisa_ssc_start(void);
void dlisa_ssc_stop(void);
void dlisa_ssc_fake_init(void);

static LCK_GRP_DECLARE(ssc_lock_group, "dlisa-ssc");

static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- what userland reads: sysctl kern.hakios.sensors ------------------------------------------------------ */

/*
 * The same structure is in userland/motion/dlisa-motion.c; `size` says whether the two agree. `gen` is odd while
 * the kernel writes: a reader takes a copy, and takes it again if gen was odd or changed meanwhile.
 */
enum { SENSOR_ACCEL, SENSOR_GYRO, SENSOR_MAG, SENSOR_LIGHT, SENSOR_PROXIMITY, SENSORS };

struct dlisa_sensors {
	uint32_t size;
	uint32_t gen;
	uint32_t source;                /* 0 nothing yet, 1 the ADSP's sensor core, 2 sensors_fake */
	uint32_t found;                 /* bit n: the sensor core has a SUID for sensor n (SENSOR_*) */
	uint32_t count[SENSORS];        /* events so far */
	uint32_t accel[3];              /* IEEE-754 float bits: m/s^2, Android's axes (x right, y up, z out of the screen) */
	uint32_t gyro[3];               /* rad/s */
	uint32_t mag[3];                /* uT */
	uint32_t light;                 /* lux */
	uint32_t proximity;             /* the sensor's distance or raw value */
	int32_t proximity_near;         /* 1 near, 0 far, -1 not known */
	uint32_t reserved;
	uint64_t accel_time;            /* mach_absolute_time() of the last accelerometer event */
};

static struct dlisa_sensors sensors = { .size = sizeof(struct dlisa_sensors), .proximity_near = -1 };
static char sensors_state[256] = "off (sensors=1 or sensors_fake=1)";

SYSCTL_DECL(_kern_hakios);
SYSCTL_OPAQUE(_kern_hakios, OID_AUTO, sensors, CTLFLAG_RD | CTLFLAG_LOCKED, &sensors, sizeof(sensors), "S,dlisa_sensors",
    "lisa's sensors: the last values (struct dlisa_sensors, userland/motion)");
SYSCTL_STRING(_kern_hakios, OID_AUTO, sensors_state, CTLFLAG_RD | CTLFLAG_LOCKED, sensors_state, sizeof(sensors_state),
    "what the sensors' client has found");

static void
publish_begin(void)
{
	os_atomic_inc(&sensors.gen, release);
}

static void
publish_end(void)
{
	os_atomic_inc(&sensors.gen, release);
}

/* ---- float bits, without floating point ----------------------------------------------------------------- */

#define F_NEG           0x80000000u
#define F_GRAVITY       0x411ce80au     /* 9.80665 */

static uint32_t
f_neg(uint32_t bits)
{
	return bits ^ F_NEG;
}

/* The value times 1000, rounded toward 0, for the log; |value| above 2^31/1000 is clamped. */
static int64_t
f_milli(uint32_t bits)
{
	int32_t exponent = (int32_t)((bits >> 23) & 0xff) - 127;
	uint64_t mantissa = (bits & 0x7fffff) | 0x800000, value;

	if (((bits >> 23) & 0xff) == 0) {
		return 0;
	}
	if (exponent > 20) {
		value = 2000000000ull;
	} else {
		value = mantissa * 1000;
		value = exponent >= 23 ? value << (exponent - 23) : value >> (23 - exponent);
	}
	return (bits & F_NEG) ? -(int64_t)value : (int64_t)value;
}

/* The sample rate's float bits for a whole number of Hz (5, 10, 25, 50, 100; anything else: 10). */
static uint32_t
f_rate(uint32_t hz)
{
	switch (hz) {
	case 5: return 0x40a00000u;
	case 25: return 0x41c80000u;
	case 50: return 0x42480000u;
	case 100: return 0x42c80000u;
	default: return 0x41200000u;    /* 10 */
	}
}

/* sensors_rot=90|180|270: the device frame turned about z, for a sensor mounted turned (the registry's "orient"). */
static void
rotate(uint32_t v[3], uint32_t degrees)
{
	uint32_t x = v[0], y = v[1];

	switch (degrees) {
	case 90: v[0] = f_neg(y); v[1] = x; break;
	case 180: v[0] = f_neg(x); v[1] = f_neg(y); break;
	case 270: v[0] = y; v[1] = f_neg(x); break;
	default: break;
	}
}

static uint32_t rotation;

/* ---- protobuf, the little that is needed ----------------------------------------------------------------- */

struct pb {
	uint8_t *data;
	uint32_t at, size;
	bool full;
};

static void
pb_byte(struct pb *b, uint8_t c)
{
	if (b->at >= b->size) {
		b->full = true;
		return;
	}
	b->data[b->at++] = c;
}

static void
pb_varint(struct pb *b, uint64_t v)
{
	do {
		pb_byte(b, (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0)));
		v >>= 7;
	} while (v != 0);
}

static void
pb_key(struct pb *b, uint32_t field, uint32_t wire)
{
	pb_varint(b, (uint64_t)field << 3 | wire);
}

static void
pb_fixed32(struct pb *b, uint32_t field, uint32_t v)
{
	pb_key(b, field, 5);
	for (unsigned i = 0; i < 4; i++) {
		pb_byte(b, (uint8_t)(v >> (8 * i)));
	}
}

static void
pb_fixed64(struct pb *b, uint32_t field, uint64_t v)
{
	pb_key(b, field, 1);
	for (unsigned i = 0; i < 8; i++) {
		pb_byte(b, (uint8_t)(v >> (8 * i)));
	}
}

static void
pb_bytes(struct pb *b, uint32_t field, const void *data, uint32_t length)
{
	pb_key(b, field, 2);
	pb_varint(b, length);
	for (uint32_t i = 0; i < length; i++) {
		pb_byte(b, ((const uint8_t *)data)[i]);
	}
}

/* One field read: its number, its wire type, and its value (varint, fixed) or its bytes (length-delimited). */
struct pb_field {
	uint32_t number, wire;
	uint64_t value;
	const uint8_t *bytes;
	uint32_t length;
};

static bool
pb_read_varint(const uint8_t *data, uint32_t size, uint32_t *at, uint64_t *v)
{
	*v = 0;
	for (unsigned shift = 0; shift < 64 && *at < size; shift += 7) {
		uint8_t c = data[(*at)++];

		*v |= (uint64_t)(c & 0x7f) << shift;
		if (!(c & 0x80)) {
			return true;
		}
	}
	return false;
}

static bool
pb_next(const uint8_t *data, uint32_t size, uint32_t *at, struct pb_field *f)
{
	uint64_t key;

	if (*at >= size || !pb_read_varint(data, size, at, &key)) {
		return false;
	}
	bzero(f, sizeof(*f));
	f->number = (uint32_t)(key >> 3);
	f->wire = (uint32_t)(key & 7);
	switch (f->wire) {
	case 0:
		return pb_read_varint(data, size, at, &f->value);
	case 1:
	case 5: {
		uint32_t n = f->wire == 1 ? 8 : 4;

		if (size - *at < n) {
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			f->value |= (uint64_t)data[*at + i] << (8 * i);
		}
		*at += n;
		return true;
	}
	case 2: {
		uint64_t length;

		if (!pb_read_varint(data, size, at, &length) || length > size - *at) {
			return false;
		}
		f->bytes = data + *at;
		f->length = (uint32_t)length;
		*at += (uint32_t)length;
		return true;
	}
	default:
		return false;           /* (groups: not in these messages) */
	}
}

/* ---- the sensor core's messages (field numbers: libssc's descriptors, see the README) ---------------------- */

#define SSC_SERVICE             400     /* QMI service "SSC" (SNS client) */
#define SSC_REQUEST             0x20    /* QMI: request and response */
#define SSC_REPORT_SMALL        0x21    /* QMI indications */
#define SSC_REPORT_LARGE        0x22
#define QMI_TYPE_REQUEST        0
#define QMI_TYPE_RESPONSE       2
#define QMI_TYPE_INDICATION     4

#define MSG_SUID_REQ            512     /* to the lookup SUID: SscSuidRequest */
#define MSG_SUID_EVENT          768     /* from it: SscSuidResponse */
#define MSG_STD_SENSOR_CONFIG   513     /* SscEnableConfigRequest: the sample rate */
#define MSG_ON_CHANGE_CONFIG    514     /* (empty): events when the value changes (light, proximity) */
#define MSG_STD_SENSOR_EVENT    1025    /* repeated float data, accuracy */
#define PROCESSOR_APSS          1
#define SUSPEND_WAKEUP          0

#define SUID_LOOKUP             0xababababababababull

struct suid {
	uint64_t low, high;
};

static const struct {
	const char *type;
	int sensor;                     /* SENSOR_*, or -1: only looked up, for the log */
	bool on_change;
} wanted_types[] = {
	{ "accel", SENSOR_ACCEL, false },
	{ "gyro", SENSOR_GYRO, false },
	{ "mag", SENSOR_MAG, false },
	{ "ambient_light", SENSOR_LIGHT, true },
	{ "proximity", SENSOR_PROXIMITY, true },
	{ "registry", -1, false },
};
#define WANTED  (sizeof(wanted_types) / sizeof(wanted_types[0]))

static struct {
	struct suid suid[WANTED];
	bool have[WANTED];
	uint32_t node, port, ours;
	uint16_t transaction;
	volatile uint32_t responses, last_result;
	bool wanted;
	volatile bool runs;
	uint32_t unknown_said;
} ssc;

/* ---- datagrams from the DSP, queued by the link's thread for ours ---------------------------------------- */

#define QUEUE           16
#define QUEUE_BYTES     4096

static LCK_MTX_DECLARE(queue_lock, &ssc_lock_group);
static struct {
	uint32_t length;
	uint8_t data[QUEUE_BYTES];
} queue[QUEUE];
static uint32_t queue_head, queue_tail, queue_dropped;

static void
ssc_receive(__unused uint32_t node, __unused uint32_t port, const uint8_t *data, uint32_t length)
{
	lck_mtx_lock(&queue_lock);
	if (queue_head - queue_tail >= QUEUE || length > QUEUE_BYTES) {
		queue_dropped++;
	} else {
		queue[queue_head % QUEUE].length = length;
		memcpy(queue[queue_head % QUEUE].data, data, length);
		queue_head++;
	}
	lck_mtx_unlock(&queue_lock);
}

/* The next datagram into `to` (QUEUE_BYTES); its length, or 0 when there is none. */
static uint32_t
ssc_take(uint8_t *to)
{
	uint32_t length = 0;

	lck_mtx_lock(&queue_lock);
	if (queue_tail != queue_head) {
		length = queue[queue_tail % QUEUE].length;
		memcpy(to, queue[queue_tail % QUEUE].data, length);
		queue_tail++;
	}
	lck_mtx_unlock(&queue_lock);
	return length;
}

/* ---- requests --------------------------------------------------------------------------------------------- */

/* sns_client_request_msg { suid, msg_id, susp_config { processor, suspend mode }, request { payload } } in a QMI request. */
static int
ssc_send(const struct suid *to, uint32_t msg_id, const uint8_t *payload, uint32_t payload_length)
{
	static uint8_t message[1024];
	uint8_t inner[256], body[512];
	struct pb b;
	uint32_t at;

	/* the request's body: the payload (field 2) */
	b = (struct pb){ inner, 0, sizeof(inner), false };
	if (payload_length != 0) {
		pb_bytes(&b, 2, payload, payload_length);
	}
	{
		uint8_t uid[32], config[16];
		struct pb u = { uid, 0, sizeof(uid), false }, c = { config, 0, sizeof(config), false };
		uint32_t inner_length = b.at;

		pb_fixed64(&u, 1, to->low);
		pb_fixed64(&u, 2, to->high);
		pb_key(&c, 1, 0);
		pb_varint(&c, PROCESSOR_APSS);
		pb_key(&c, 2, 0);
		pb_varint(&c, SUSPEND_WAKEUP);
		b = (struct pb){ body, 0, sizeof(body), false };
		pb_bytes(&b, 1, uid, u.at);
		pb_fixed32(&b, 2, msg_id);
		pb_bytes(&b, 3, config, c.at);
		pb_bytes(&b, 4, inner, inner_length);
		if (b.full || u.full || c.full) {
			return EMSGSIZE;
		}
	}
	/* QMI: type, transaction, message, length; then TLV 1: a 16-bit length and the bytes */
	ssc.transaction = (uint16_t)(ssc.transaction + 1 == 0 ? 1 : ssc.transaction + 1);
	at = 0;
	message[at++] = QMI_TYPE_REQUEST;
	message[at++] = (uint8_t)ssc.transaction;
	message[at++] = (uint8_t)(ssc.transaction >> 8);
	message[at++] = SSC_REQUEST;
	message[at++] = 0;
	message[at++] = (uint8_t)(3 + 2 + b.at);
	message[at++] = (uint8_t)((3 + 2 + b.at) >> 8);
	message[at++] = 0x01;
	message[at++] = (uint8_t)(2 + b.at);
	message[at++] = (uint8_t)((2 + b.at) >> 8);
	message[at++] = (uint8_t)b.at;
	message[at++] = (uint8_t)(b.at >> 8);
	memcpy(message + at, body, b.at);
	at += b.at;
	return dlisa_adsp_qrtr_send(ssc.ours, ssc.node, ssc.port, message, at);
}

static const struct suid lookup = { SUID_LOOKUP, SUID_LOOKUP };

static int
ssc_ask_suid(const char *type)
{
	uint8_t payload[64];
	struct pb b = { payload, 0, sizeof(payload), false };

	pb_bytes(&b, 1, type, (uint32_t)strlen(type));
	pb_key(&b, 2, 0);               /* enable_updates: tell of sensors that appear later too */
	pb_varint(&b, 1);
	return ssc_send(&lookup, MSG_SUID_REQ, payload, b.at);
}

static int
ssc_enable(unsigned w, uint32_t hz)
{
	uint8_t payload[16];
	struct pb b = { payload, 0, sizeof(payload), false };

	if (wanted_types[w].on_change) {
		return ssc_send(&ssc.suid[w], MSG_ON_CHANGE_CONFIG, NULL, 0);
	}
	pb_fixed32(&b, 1, f_rate(hz));  /* sample_rate, a float */
	return ssc_send(&ssc.suid[w], MSG_STD_SENSOR_CONFIG, payload, b.at);
}

/* ---- what comes back ------------------------------------------------------------------------------------- */

static void
on_suid_event(const uint8_t *data, uint32_t size)
{
	struct pb_field f;
	uint32_t at = 0;
	char type[33] = "";
	struct suid first = { 0, 0 };
	unsigned uids = 0;

	while (pb_next(data, size, &at, &f)) {
		if (f.number == 1 && f.wire == 2) {
			memcpy(type, f.bytes, f.length < sizeof(type) - 1 ? f.length : sizeof(type) - 1);
		} else if (f.number == 2 && f.wire == 2) {
			struct pb_field g;
			uint32_t in = 0;
			struct suid s = { 0, 0 };

			while (pb_next(f.bytes, f.length, &in, &g)) {
				s.low = g.number == 1 ? g.value : s.low;
				s.high = g.number == 2 ? g.value : s.high;
			}
			first = uids++ == 0 ? s : first;
		}
	}
	tell("ssc: the sensor core has %u sensor%s of type \"%s\"%s\n", uids, uids == 1 ? "" : "s", type,
	    uids ? "" : " (none)");
	for (unsigned w = 0; w < WANTED && uids != 0; w++) {
		if (strcmp(type, wanted_types[w].type) == 0 && !ssc.have[w]) {
			ssc.suid[w] = first;
			ssc.have[w] = true;
			tell("ssc:   %s: SUID %016llx%016llx\n", type, first.high, first.low);
			if (wanted_types[w].sensor >= 0) {
				publish_begin();
				sensors.found |= 1u << wanted_types[w].sensor;
				publish_end();
			}
		}
	}
}

static void
on_sensor_event(unsigned w, uint32_t msg_id, const uint8_t *data, uint32_t size)
{
	int sensor = wanted_types[w].sensor;
	uint32_t values[4] = { 0 }, count = 0, varint_one = 0, varint_two = 0;
	bool varints = false;
	struct pb_field f;
	uint32_t at = 0;

	while (pb_next(data, size, &at, &f)) {
		if (f.number == 1 && f.wire == 5 && count < 4) {
			values[count++] = (uint32_t)f.value;
		} else if (f.number == 1 && f.wire == 2) {
			for (uint32_t i = 0; i + 4 <= f.length && count < 4; i += 4) {      /* (packed floats) */
				values[count++] = (uint32_t)(f.bytes[i] | f.bytes[i + 1] << 8 | f.bytes[i + 2] << 16 |
				    (uint32_t)f.bytes[i + 3] << 24);
			}
		} else if (f.number == 1 && f.wire == 0) {
			varints = true;
			varint_one = (uint32_t)f.value;
		} else if (f.number == 2 && f.wire == 0) {
			varint_two = (uint32_t)f.value;
		}
	}
	if (sensor < 0 || (msg_id != MSG_STD_SENSOR_EVENT && !(sensor == SENSOR_PROXIMITY && varints))) {
		if (ssc.unknown_said++ < 12) {
			tell("ssc: %s: message %u of %u bytes (not a sensor event this reads)\n", wanted_types[w].type, msg_id, size);
		}
		return;
	}
	publish_begin();
	sensors.source = 1;
	sensors.count[sensor]++;
	switch (sensor) {
	case SENSOR_ACCEL:
		if (count >= 3) {
			memcpy(sensors.accel, values, sizeof(sensors.accel));
			rotate(sensors.accel, rotation);
			sensors.accel_time = mach_absolute_time();
		}
		break;
	case SENSOR_GYRO:
		if (count >= 3) {
			memcpy(sensors.gyro, values, sizeof(sensors.gyro));
			rotate(sensors.gyro, rotation);
		}
		break;
	case SENSOR_MAG:
		if (count >= 3) {
			memcpy(sensors.mag, values, sizeof(sensors.mag));
			rotate(sensors.mag, rotation);
		}
		break;
	case SENSOR_LIGHT:
		sensors.light = count ? values[0] : sensors.light;
		break;
	case SENSOR_PROXIMITY:
		/* libssc's SscProximityResponse: near (1, int32), distance (2); or a standard event's floats */
		if (varints) {
			sensors.proximity_near = varint_one != 0;
			sensors.proximity = varint_two;
		} else if (count) {
			sensors.proximity = values[0];
		}
		break;
	default:
		break;
	}
	publish_end();
}

/* SscClientResponse { uid, repeated { msg_id, timestamp, msg } } */
static void
on_report(const uint8_t *data, uint32_t size)
{
	struct pb_field f;
	struct suid from = { 0, 0 };
	uint32_t at = 0;

	/* (the SUID comes first, but is looked for before the events in case it does not) */
	while (pb_next(data, size, &at, &f)) {
		if (f.number == 1 && f.wire == 2) {
			struct pb_field g;
			uint32_t in = 0;

			while (pb_next(f.bytes, f.length, &in, &g)) {
				from.low = g.number == 1 ? g.value : from.low;
				from.high = g.number == 2 ? g.value : from.high;
			}
		}
	}
	at = 0;
	while (pb_next(data, size, &at, &f)) {
		struct pb_field g;
		uint32_t in = 0, msg_id = 0, msg_length = 0;
		const uint8_t *msg = NULL;

		if (f.number != 2 || f.wire != 2) {
			continue;
		}
		while (pb_next(f.bytes, f.length, &in, &g)) {
			if (g.number == 1) {
				msg_id = (uint32_t)g.value;
			} else if (g.number == 3 && g.wire == 2) {
				msg = g.bytes;
				msg_length = g.length;
			}
		}
		if (from.low == SUID_LOOKUP && from.high == SUID_LOOKUP) {
			if (msg_id == MSG_SUID_EVENT && msg != NULL) {
				on_suid_event(msg, msg_length);
			}
			continue;
		}
		for (unsigned w = 0; w < WANTED; w++) {
			if (ssc.have[w] && ssc.suid[w].low == from.low && ssc.suid[w].high == from.high) {
				on_sensor_event(w, msg_id, msg ? msg : (const uint8_t *)"", msg_length);
			}
		}
	}
}

static void
dump(const char *what, const uint8_t *data, uint32_t length)
{
	char line[200];
	int at = snprintf(line, sizeof(line), "ssc: %s (%u bytes):", what, length);

	for (uint32_t i = 0; i < length && i < 40 && at < (int)sizeof(line) - 4; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", data[i]);
	}
	tell("%s\n", line);
}

/* One QMI message from the service: a response to our request, or a report. */
static void
on_message(const uint8_t *data, uint32_t length)
{
	static unsigned dumped;
	uint32_t message, at = 7;

	if (length < 7) {
		return;
	}
	message = (uint32_t)(data[3] | data[4] << 8);
	if (dumped < 6) {
		dumped++;
		dump(data[0] == QMI_TYPE_RESPONSE ? "< response" : data[0] == QMI_TYPE_INDICATION ? "< indication" : "< message",
		    data, length);
	}
	/* TLVs: type (1), length (2), value */
	while (at + 3 <= length) {
		uint8_t type = data[at];
		uint32_t size = (uint32_t)(data[at + 1] | data[at + 2] << 8);
		const uint8_t *value = data + at + 3;

		if (size > length - at - 3) {
			break;
		}
		if (data[0] == QMI_TYPE_RESPONSE && message == SSC_REQUEST && type == 0x02 && size >= 4) {
			ssc.last_result = (uint32_t)(value[0] | value[1] << 8 | value[2] << 16 | (uint32_t)value[3] << 24);
			os_atomic_inc(&ssc.responses, release);
			if (ssc.last_result != 0) {
				tell("ssc: the request is refused: result %u, error %u\n", value[0] | value[1] << 8, value[2] | value[3] << 8);
			}
		}
		if (data[0] == QMI_TYPE_INDICATION && (message == SSC_REPORT_SMALL || message == SSC_REPORT_LARGE) && type == 0x02 &&
		    size >= 2) {
			uint32_t inner = (uint32_t)(value[0] | value[1] << 8);

			if (inner <= size - 2) {
				on_report(value + 2, inner);
			}
		}
		at += 3 + size;
	}
}

#define ANY_TIME        (~0u)           /* pump(): no response awaited, only `ms` of handling what comes */

/* What comes is handled for up to `ms`; true as soon as the response count has moved past `after`. */
static bool
pump(uint32_t ms, uint32_t after)
{
	static uint8_t message[QUEUE_BYTES];

	for (uint32_t waited = 0;; waited += 5) {
		uint32_t length;

		while ((length = ssc_take(message)) != 0) {
			on_message(message, length);
		}
		if (after != ANY_TIME && os_atomic_load(&ssc.responses, acquire) != after) {
			return true;
		}
		if (waited >= ms || !ssc.wanted) {
			return false;
		}
		sleep_ms(5);
	}
}

/* ---- the client's thread ------------------------------------------------------------------------------------ */

static void
state(const char *format, ...) __printflike(1, 2);

static void
state(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	vsnprintf(sensors_state, sizeof(sensors_state), format, list);
	va_end(list);
}

static void
log_values(void)
{
	tell("ssc: events: accel %u gyro %u mag %u light %u proximity %u (dropped %u); accel %lld %lld %lld mm/s2, light %lld mlx, "
	    "proximity %s\n", sensors.count[SENSOR_ACCEL], sensors.count[SENSOR_GYRO], sensors.count[SENSOR_MAG],
	    sensors.count[SENSOR_LIGHT], sensors.count[SENSOR_PROXIMITY], queue_dropped, f_milli(sensors.accel[0]),
	    f_milli(sensors.accel[1]), f_milli(sensors.accel[2]), f_milli(sensors.light),
	    sensors.proximity_near < 0 ? "not known" : sensors.proximity_near ? "near" : "far");
}

static void
ssc_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t wait_seconds = 90, hz = 10, instance = 0, before;

	ssc.runs = true;
	PE_parse_boot_argn("sensors_wait", &wait_seconds, sizeof(wait_seconds));
	PE_parse_boot_argn("sensors_rate", &hz, sizeof(hz));
	PE_parse_boot_argn("sensors_rot", &rotation, sizeof(rotation));
	state("waiting for the sensor core (QMI service %u) on the ADSP", SSC_SERVICE);
	for (uint32_t ms = 0; ssc.wanted && ms < wait_seconds * 1000; ms += 100) {
		if (dlisa_adsp_up() && dlisa_adsp_qrtr_service(SSC_SERVICE, &ssc.node, &ssc.port, &instance)) {
			break;
		}
		sleep_ms(100);
	}
	if (!ssc.wanted || ssc.port == 0) {
		tell("ssc: NO SENSOR CORE: the DSP has not told of QMI service %u in %u s (adsp: qrtr: a service ... lists what it "
		    "did tell of); the sensors PD probably waits for FastRPC (README.md)\n", SSC_SERVICE, wait_seconds);
		state("no QMI service %u on the ADSP after %u s", SSC_SERVICE, wait_seconds);
		goto out;
	}
	ssc.ours = dlisa_adsp_qrtr_open(ssc_receive);
	tell("ssc: the sensor core is QMI service %u, version %u, instance %u, at node %u port %u; our port %u\n", SSC_SERVICE,
	    instance & 0xff, instance >> 8, ssc.node, ssc.port, ssc.ours);
	if (ssc.ours == 0) {
		state("no QRTR port");
		goto out;
	}
	for (unsigned w = 0; w < WANTED && ssc.wanted; w++) {
		int error;

		before = os_atomic_load(&ssc.responses, acquire);
		error = ssc_ask_suid(wanted_types[w].type);
		if (error != 0 || !pump(2000, before)) {
			tell("ssc: the SUID question for \"%s\": %s (%d)\n", wanted_types[w].type, error ? "NOT SENT" : "NO RESPONSE", error);
			continue;
		}
		pump(1500, ANY_TIME);   /* (the answer is a report after the response: give it time) */
	}
	state("service %u at %u:%u; SUIDs:%s%s%s%s%s%s", SSC_SERVICE, ssc.node, ssc.port, ssc.have[0] ? " accel" : "",
	    ssc.have[1] ? " gyro" : "", ssc.have[2] ? " mag" : "", ssc.have[3] ? " light" : "", ssc.have[4] ? " proximity" : "",
	    ssc.have[5] ? " registry" : " (NO registry)");
	tell("ssc: %s\n", sensors_state);
	for (unsigned w = 0; w < WANTED && ssc.wanted; w++) {
		int error;

		if (!ssc.have[w] || wanted_types[w].sensor < 0) {
			continue;
		}
		before = os_atomic_load(&ssc.responses, acquire);
		error = ssc_enable(w, hz);
		tell("ssc: %s enabled (%s): %s\n", wanted_types[w].type, wanted_types[w].on_change ? "on change" : "streaming",
		    error ? "NOT SENT" : pump(2000, before) ? (ssc.last_result == 0 ? "accepted" : "REFUSED") : "NO RESPONSE");
	}
	for (unsigned round = 0; ssc.wanted; round++) {
		pump(100, ANY_TIME);
		if (round % 100 == 99) {
			log_values();
		}
	}
out:
	if (ssc.ours != 0) {
		dlisa_adsp_qrtr_close(ssc.ours);
		ssc.ours = 0;
	}
	ssc.runs = false;
	thread_terminate(current_thread());
}

/* From hakios.adsp.c, once the DSP is up and linked. */
void
dlisa_ssc_start(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sensors", &on, sizeof(on)) || on == 0 || ssc.runs) {
		return;
	}
	ssc.wanted = true;
	if (kernel_thread_start(ssc_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
dlisa_ssc_stop(void)
{
	ssc.wanted = false;
	for (unsigned ms = 0; ms < 2000 && ssc.runs; ms++) {
		sleep_ms(1);
	}
}

/* ---- sensors_fake=1 ---------------------------------------------------------------------------------------- */

static void
fake_thread(__unused void *parameter, __unused wait_result_t result)
{
	/* portrait, landscape (bottom to the right: UIDeviceOrientationLandscapeLeft), upside down, landscape (bottom left) */
	static const uint32_t gravity[4][3] = {
		{ 0, F_GRAVITY, 0 }, { F_GRAVITY, 0, 0 }, { 0, F_GRAVITY ^ F_NEG, 0 }, { F_GRAVITY ^ F_NEG, 0, 0 },
	};
	uint32_t period = 15;

	PE_parse_boot_argn("sensors_fake_secs", &period, sizeof(period));
	period = period < 2 ? 2 : period;
	tell("ssc: sensors_fake=1: a made-up gravity vector, a quarter turn every %u s\n", period);
	state("sensors_fake=1: made-up values");
	for (uint64_t tick = 0;; tick++) {
		unsigned phase = (unsigned)((tick / (period * 10)) % 4);

		publish_begin();
		sensors.source = 2;
		sensors.found = 1u << SENSOR_ACCEL | 1u << SENSOR_LIGHT | 1u << SENSOR_PROXIMITY;
		memcpy(sensors.accel, gravity[phase], sizeof(sensors.accel));
		sensors.accel_time = mach_absolute_time();
		sensors.count[SENSOR_ACCEL]++;
		sensors.light = 0x42c80000u;    /* 100 lux */
		sensors.proximity_near = 0;
		publish_end();
		if (tick % (period * 10) == 0) {
			tell("ssc: sensors_fake: phase %u (accel %lld %lld %lld mm/s2)\n", phase, f_milli(sensors.accel[0]),
			    f_milli(sensors.accel[1]), f_milli(sensors.accel[2]));
		}
		sleep_ms(100);
	}
}

/*
 * sensors_fake=2: the made-up values only if the real accelerometer has given nothing by the time the client would
 * have found it (sensors_wait + 60 s after the kernel's start). One boot then tests both: the ADSP's sensors if they
 * work, and the iOS side's rotation if they do not.
 */
static void
fallback_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t wait_seconds = 90;

	PE_parse_boot_argn("sensors_wait", &wait_seconds, sizeof(wait_seconds));
	sleep_ms((wait_seconds + 60) * 1000);
	if (sensors.count[SENSOR_ACCEL] != 0) {
		tell("ssc: sensors_fake=2: the accelerometer works (%u events): no made-up values\n", sensors.count[SENSOR_ACCEL]);
		thread_terminate(current_thread());
		return;
	}
	tell("ssc: sensors_fake=2: no accelerometer event after %u s: made-up values from now on (%s)\n", wait_seconds + 60,
	    sensors_state);
	fake_thread(NULL, THREAD_AWAKENED);
}

/* From hakios_adsp_init(), whatever adsp= says. */
void
dlisa_ssc_fake_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sensors_fake", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(on == 2 ? fallback_thread : fake_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
