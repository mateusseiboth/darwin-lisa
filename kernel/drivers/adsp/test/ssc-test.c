/*
 * Host test of dlisa.ssc.c's wire formats: the QMI request it sends and the reports it reads, built here byte by
 * byte from the schema in libssc's protobuf-c descriptors (README.md) and libqmi's SSC service, independently of the
 * driver's own encoder. Run: sh run-tests.sh (builds with the host cc against shim/, runs, exits non-zero on failure).
 */
#include "../dlisa.ssc.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- stand-ins for the kernel and for hakios.adsp.c ---- */

static uint32_t boot_rotation;
bool
PE_parse_boot_argn(const char *name, void *value, int size)
{
	if (strcmp(name, "sensors_rot") == 0 && boot_rotation != 0 && size == 4) {
		memcpy(value, &boot_rotation, 4);
		return true;
	}
	return false;
}
void loader_log_putc(char c) { (void)c; }
kern_return_t thread_terminate(thread_t t) { (void)t; return 0; }
bool dlisa_adsp_up(void) { return true; }
bool dlisa_adsp_qrtr_service(uint32_t s, uint32_t *n, uint32_t *p, uint32_t *i) { (void)s; *n = 5; *p = 12; *i = 1; return true; }
uint32_t dlisa_adsp_qrtr_open(dlisa_adsp_receive_t r) { (void)r; return 0x4000; }
void dlisa_adsp_qrtr_close(uint32_t n) { (void)n; }

static uint8_t sent[2048];
static uint32_t sent_length, sent_to_port;
int
dlisa_adsp_qrtr_send(uint32_t from, uint32_t node, uint32_t port, const void *data, uint32_t length)
{
	(void)from; (void)node;
	sent_to_port = port;
	memcpy(sent, data, length);
	sent_length = length;
	return 0;
}

/* ---- a byte builder, written apart from the driver's encoder ---- */

struct bytes {
	uint8_t b[1024];
	uint32_t n;
};

static void put(struct bytes *x, const void *data, uint32_t n) { memcpy(x->b + x->n, data, n); x->n += n; }
static void put1(struct bytes *x, uint8_t c) { x->b[x->n++] = c; }
static void put32(struct bytes *x, uint32_t v) { for (int i = 0; i < 4; i++) put1(x, (uint8_t)(v >> (8 * i))); }
static void put64(struct bytes *x, uint64_t v) { for (int i = 0; i < 8; i++) put1(x, (uint8_t)(v >> (8 * i))); }
/* a length-delimited field whose length fits one byte */
static void field_bytes(struct bytes *x, uint8_t field, const struct bytes *inner) { put1(x, (uint8_t)(field << 3 | 2)); put1(x, (uint8_t)inner->n); put(x, inner->b, inner->n); }

static void
uid(struct bytes *x, uint64_t low, uint64_t high)
{
	put1(x, 0x09); put64(x, low);           /* 1: fixed64 */
	put1(x, 0x11); put64(x, high);          /* 2: fixed64 */
}

/* SscClientResponse { uid, response { msg_id, timestamp, msg } } in a QMI indication 0x21 */
static uint32_t
indication(uint8_t *out, uint64_t low, uint64_t high, uint32_t msg_id, const struct bytes *msg)
{
	struct bytes u = { .n = 0 }, r = { .n = 0 }, top = { .n = 0 }, q = { .n = 0 };

	uid(&u, low, high);
	put1(&r, 0x0d); put32(&r, msg_id);      /* 1: fixed32 */
	put1(&r, 0x11); put64(&r, 777);         /* 2: fixed64 timestamp */
	field_bytes(&r, 3, msg);
	field_bytes(&top, 1, &u);
	field_bytes(&top, 2, &r);
	put1(&q, 4); put1(&q, 9); put1(&q, 0); put1(&q, 0x21); put1(&q, 0);
	put1(&q, (uint8_t)(11 + 5 + top.n)); put1(&q, 0);
	put1(&q, 0x01); put1(&q, 8); put1(&q, 0); put64(&q, 42);          /* TLV 1: client id */
	put1(&q, 0x02); put1(&q, (uint8_t)(2 + top.n)); put1(&q, 0);     /* TLV 2: the data, a 16-bit length first */
	put1(&q, (uint8_t)top.n); put1(&q, 0); put(&q, top.b, top.n);
	memcpy(out, q.b, q.n);
	return q.n;
}

int
main(void)
{
	uint8_t message[1024];
	uint32_t n;

	/* float bits -> thousandths */
	CHECK(f_milli(F_GRAVITY) == 9806, "f_milli(9.80665) = %lld", f_milli(F_GRAVITY));
	CHECK(f_milli(F_GRAVITY ^ F_NEG) == -9806, "f_milli(-9.80665) = %lld", f_milli(F_GRAVITY ^ F_NEG));
	CHECK(f_milli(0x42c80000u) == 100000, "f_milli(100) = %lld", f_milli(0x42c80000u));
	CHECK(f_milli(0x3f000000u) == 500, "f_milli(0.5) = %lld", f_milli(0x3f000000u));
	CHECK(f_milli(0) == 0, "f_milli(0)");

	/* the SUID question for "accel": the bytes, as the schema says they are */
	ssc.ours = 0x4000; ssc.node = 5; ssc.port = 12; ssc.transaction = 0;
	CHECK(ssc_ask_suid("accel") == 0, "ssc_ask_suid");
	{
		struct bytes u = { .n = 0 }, cfg = { .n = 0 }, sreq = { .n = 0 }, req = { .n = 0 }, body = { .n = 0 }, q = { .n = 0 };

		uid(&u, SUID_LOOKUP, SUID_LOOKUP);
		put1(&cfg, 0x08); put1(&cfg, 1); put1(&cfg, 0x10); put1(&cfg, 0);  /* processor 1 (APSS), suspend mode 0 */
		put1(&sreq, 0x0a); put1(&sreq, 5); put(&sreq, "accel", 5); put1(&sreq, 0x10); put1(&sreq, 1);
		field_bytes(&req, 2, &sreq);
		field_bytes(&body, 1, &u);
		put1(&body, 0x15); put32(&body, 512);
		field_bytes(&body, 3, &cfg);
		field_bytes(&body, 4, &req);
		put1(&q, 0); put1(&q, 1); put1(&q, 0); put1(&q, 0x20); put1(&q, 0);
		put1(&q, (uint8_t)(5 + body.n)); put1(&q, 0);
		put1(&q, 0x01); put1(&q, (uint8_t)(2 + body.n)); put1(&q, 0); put1(&q, (uint8_t)body.n); put1(&q, 0);
		put(&q, body.b, body.n);
		CHECK(sent_length == q.n && memcmp(sent, q.b, q.n) == 0, "SUID request: %u bytes sent, %u expected", sent_length, q.n);
		CHECK(sent_to_port == 12, "sent to port %u", sent_to_port);
	}

	/* a response: result TLV 2 = 0 */
	{
		uint8_t r[] = { 2, 1, 0, 0x20, 0, 7, 0, 0x02, 4, 0, 0, 0, 0, 0 };
		uint32_t before = ssc.responses;

		on_message(r, sizeof(r));
		CHECK(ssc.responses == before + 1 && ssc.last_result == 0, "response counted");
	}

	/* the lookup's answer: one accelerometer, SUID 0x2222:0x1111 */
	{
		struct bytes s = { .n = 0 }, u = { .n = 0 };

		put1(&s, 0x0a); put1(&s, 5); put(&s, "accel", 5);
		uid(&u, 0x1111, 0x2222);
		field_bytes(&s, 2, &u);
		n = indication(message, SUID_LOOKUP, SUID_LOOKUP, MSG_SUID_EVENT, &s);
		on_message(message, n);
		CHECK(ssc.have[0] && ssc.suid[0].low == 0x1111 && ssc.suid[0].high == 0x2222, "accel SUID taken");
		CHECK(sensors.found & 1, "found bit");
	}

	/* the enable request for the accelerometer: msg 513 to its SUID, sample rate 10.0f */
	CHECK(ssc_enable(0, 10) == 0, "enable");
	{
		/* body: 1 uid(0x1111,0x2222), 2 fixed32 513, 3 config, 4 request{2: {0x0d, 10.0f}} */
		CHECK(sent_length > 20 && memmem(sent, sent_length, "\x15\x01\x02\x00\x00", 5) != NULL, "msg id 513 in the enable");
		CHECK(memmem(sent, sent_length, "\x0d\x00\x00\x20\x41", 5) != NULL, "sample rate 10.0f in the enable");
	}

	/* an accelerometer event, floats unpacked */
	{
		struct bytes e = { .n = 0 };

		put1(&e, 0x0d); put32(&e, 0x3f800000u);        /* 1.0 */
		put1(&e, 0x0d); put32(&e, F_GRAVITY);          /* 9.80665 */
		put1(&e, 0x0d); put32(&e, 0xbf000000u);        /* -0.5 */
		put1(&e, 0x10); put1(&e, 3);                   /* accuracy */
		n = indication(message, 0x1111, 0x2222, MSG_STD_SENSOR_EVENT, &e);
		on_message(message, n);
		CHECK(sensors.count[SENSOR_ACCEL] == 1, "accel count %u", sensors.count[SENSOR_ACCEL]);
		CHECK(sensors.accel[0] == 0x3f800000u && sensors.accel[1] == F_GRAVITY && sensors.accel[2] == 0xbf000000u,
		    "accel values %08x %08x %08x", sensors.accel[0], sensors.accel[1], sensors.accel[2]);
		CHECK(sensors.source == 1 && (sensors.gen & 1) == 0, "source %u gen %u", sensors.source, sensors.gen);
	}

	/* the same, packed, and turned 90 degrees: (x, y) -> (-y, x) */
	{
		struct bytes e = { .n = 0 }, floats = { .n = 0 };

		put32(&floats, 0x3f800000u); put32(&floats, F_GRAVITY); put32(&floats, 0);
		field_bytes(&e, 1, &floats);
		rotation = 90;
		n = indication(message, 0x1111, 0x2222, MSG_STD_SENSOR_EVENT, &e);
		on_message(message, n);
		rotation = 0;
		CHECK(sensors.accel[0] == (F_GRAVITY ^ F_NEG) && sensors.accel[1] == 0x3f800000u, "packed + rotated: %08x %08x",
		    sensors.accel[0], sensors.accel[1]);
	}

	/* proximity, libssc's SscProximityResponse { near, distance, accuracy } */
	{
		struct bytes s = { .n = 0 }, u = { .n = 0 }, e = { .n = 0 };

		put1(&s, 0x0a); put1(&s, 9); put(&s, "proximity", 9);
		uid(&u, 0x3333, 0x4444);
		field_bytes(&s, 2, &u);
		n = indication(message, SUID_LOOKUP, SUID_LOOKUP, MSG_SUID_EVENT, &s);
		on_message(message, n);
		CHECK(ssc.have[4], "proximity SUID taken");
		put1(&e, 0x08); put1(&e, 1); put1(&e, 0x10); put1(&e, 5); put1(&e, 0x18); put1(&e, 3);
		n = indication(message, 0x3333, 0x4444, 774, &e);
		on_message(message, n);
		CHECK(sensors.proximity_near == 1 && sensors.proximity == 5, "proximity near %d distance %u", sensors.proximity_near,
		    sensors.proximity);
	}

	/*
	 * sensors the core tells of after the first round (with sensors_rpc=1 the registry is still being read over FastRPC
	 * then): ssc_enable_new() enables each found and not yet enabled one, once
	 */
	{
		struct bytes s = { .n = 0 }, u = { .n = 0 };
		uint32_t enabled;

		ssc.enabled[0] = true;                          /* (the accelerometer, enabled above) */
		ssc.wanted = true;                              /* (as on the client's thread) */
		put1(&s, 0x0a); put1(&s, 4); put(&s, "gyro", 4);
		uid(&u, 0x5555, 0x6666);
		field_bytes(&s, 2, &u);
		n = indication(message, SUID_LOOKUP, SUID_LOOKUP, MSG_SUID_EVENT, &s);
		on_message(message, n);
		CHECK(ssc.have[1], "gyro SUID taken later");
		enabled = ssc_enable_new(10);
		CHECK(enabled == 2, "the gyro and the proximity sensor enabled: %u", enabled);
		CHECK(ssc.enabled[1] && ssc.enabled[4] && !ssc.enabled[2], "enabled: the found ones only");
		CHECK(ssc_enable_new(10) == 0, "none twice");
		CHECK(ssc_missing() == (1u << 2 | 1u << 3 | 1u << 5), "still missing: mag, light, registry (%#x)", ssc_missing());
		ssc.wanted = false;
	}

	/* a truncated message must not be read past its end */
	{
		n = indication(message, 0x1111, 0x2222, MSG_STD_SENSOR_EVENT, &(struct bytes){ .n = 0 });
		on_message(message, n - 3);
		on_message(message, 5);
	}

	printf("%s: %d failure%s\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
	return failures != 0;
}
