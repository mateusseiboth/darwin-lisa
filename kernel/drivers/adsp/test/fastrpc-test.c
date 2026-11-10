/*
 * Host test of dlisa.fastrpc_proto.h: the FastRPC wire formats and the file server the sensors PD reads its
 * configuration through. Every expected byte here is built by hand from the references (Linux's
 * drivers/misc/fastrpc.c, Qualcomm's fastrpc listener_buf.h / apps_std.idl / remotectl.idl / adsp_listener.idl,
 * hexagonrpcd's method tables), independently of the code under test.
 * Run: sh run-fastrpc-tests.sh (host cc, UBSan traps; exits non-zero on failure).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dlisa.fastrpc_proto.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* ---- the host's side of the server's callbacks ---- */

static uint32_t allocated, freed_count;
static void *test_alloc(void *ctx, uint32_t size) { (void)ctx; allocated += size; return calloc(1, size ? size : 1); }
static void test_free(void *ctx, void *p, uint32_t size) { (void)ctx; allocated -= size; freed_count++; free(p); }
static char last_log[512];
static uint32_t logs;
static void test_log(void *ctx, const char *line) { (void)ctx; logs++; snprintf(last_log, sizeof(last_log), "%s", line); }
static uint64_t mapped_length;
static uint32_t mapped_flags;
static uint32_t
test_map(void *ctx, uint64_t length, uint32_t flags, uint64_t *dsp_address)
{
	(void)ctx;
	mapped_length = length;
	mapped_flags = flags;
	*dsp_address = 0xe0001000ull;
	return 0;
}

static const struct frpc_ops ops = { test_alloc, test_free, test_log, test_map, NULL };

/* ---- an archive, written here byte by byte (the format the staging script writes) ---- */

struct file { const char *path; const char *data; uint32_t size; };

static uint8_t *
make_archive(const struct file *files, uint32_t count, uint32_t *size)
{
	uint32_t at = 16 + 16 * count, total = at;
	uint8_t *a;

	for (uint32_t i = 0; i < count; i++) {
		total += (uint32_t)strlen(files[i].path) + 1 + files[i].size;
	}
	a = calloc(1, total);
	memcpy(a, "DLSNSFS1", 8);
	wr32(a + 8, count);
	wr32(a + 12, total);
	for (uint32_t i = 0; i < count; i++) {
		uint32_t length = (uint32_t)strlen(files[i].path);

		wr32(a + 16 + 16 * i, at);
		wr32(a + 20 + 16 * i, length);
		memcpy(a + at, files[i].path, length + 1);
		at += length + 1;
		wr32(a + 24 + 16 * i, at);
		wr32(a + 28 + 16 * i, files[i].size);
		memcpy(a + at, files[i].data, files[i].size);
		at += files[i].size;
	}
	*size = total;
	return a;
}

static char big[5000];
static const struct file tree[] = {
	{ "/mnt/vendor/persist/sensors/registry/registry/icm4x6xx_0", "{\"a\":1}", 7 },
	{ "/mnt/vendor/persist/sensors/registry/registry/stk3a6x_0", "{\"b\":2}", 7 },
	{ "/mnt/vendor/persist/sensors/registry/sns_reg_version", "version=6\n", 10 },
	{ "/sys/devices/soc0/soc_id", "475\n", 4 },
	{ "/vendor/etc/sensors/config/sm7350_icm4x6xx_0.json", big, sizeof(big) },
	{ "/vendor/etc/sensors/sns_reg_config", "version=6\n", 10 },
};
#define TREE (sizeof(tree) / sizeof(tree[0]))

/* ---- one reverse call, as the listener hands it over ---- */

struct call {
	uint8_t prim_in[64];
	uint32_t prim_in_size;
	const void *in[4];
	uint32_t in_size[4];
	uint32_t in_count;              /* besides the primary one */
	uint32_t out_size[4];           /* [0]: the primary one */
	uint32_t out_count;
	uint8_t out[4][512];
	uint32_t result;
};

static void
prim_word(struct call *c, uint32_t v)
{
	wr32(c->prim_in + c->prim_in_size, v);
	c->prim_in_size += 4;
}

static void
prim_word64(struct call *c, uint64_t v)
{
	wr64(c->prim_in + c->prim_in_size, v);
	c->prim_in_size += 8;
}

static void
in_string(struct call *c, const char *s)
{
	c->in[c->in_count] = s;
	c->in_size[c->in_count] = (uint32_t)strlen(s) + 1;
	c->in_count++;
	prim_word(c, (uint32_t)strlen(s) + 1);
}

static uint32_t
serve(struct frpc_server *s, uint32_t handle, uint32_t method, struct call *c)
{
	struct frpc_view in[5];
	struct frpc_buf out[4];
	uint32_t n_in = c->prim_in_size ? 1 : 0;

	memset(c->out, 0xee, sizeof(c->out));
	if (c->prim_in_size) {
		in[0].data = c->prim_in;
		in[0].length = c->prim_in_size;
	}
	for (uint32_t i = 0; i < c->in_count; i++) {
		in[n_in].data = c->in[i];
		in[n_in].length = c->in_size[i];
		n_in++;
	}
	for (uint32_t i = 0; i < c->out_count; i++) {
		out[i].data = c->out[i];
		out[i].length = c->out_size[i];
	}
	c->result = frpc_server_call(s, handle, FRPC_SC(method, n_in, c->out_count), in, out);
	return c->result;
}

static uint32_t
open_interface(struct frpc_server *s, const char *name, uint32_t *handle)
{
	struct call c = { 0 };

	in_string(&c, name);
	prim_word(&c, 64);              /* dlerror's length */
	c.out_count = 2;
	c.out_size[0] = 8;
	c.out_size[1] = 64;
	serve(s, 0, 0, &c);
	*handle = rd32(c.out[0]);
	CHECK(c.out[1][0] == 0, "dlerror is an empty string");
	return c.result ? c.result : rd32(c.out[0] + 4);
}

static uint32_t
std_open(struct frpc_server *s, uint32_t std, const char *name, const char *mode, uint32_t *fd)
{
	struct call c = { 0 };

	in_string(&c, "ADSP_LIBRARY_PATH");
	in_string(&c, ";");
	in_string(&c, name);
	in_string(&c, mode);
	c.out_count = 1;
	c.out_size[0] = 4;
	serve(s, std, 19, &c);
	*fd = rd32(c.out[0]);
	return c.result;
}

static uint32_t
std_read(struct frpc_server *s, uint32_t std, uint32_t fd, uint32_t length, uint8_t *to, uint32_t *got, uint32_t *eof)
{
	struct call c = { 0 };

	prim_word(&c, fd);
	prim_word(&c, length);
	c.out_count = 2;
	c.out_size[0] = 8;
	c.out_size[1] = length;
	serve(s, std, 4, &c);
	*got = rd32(c.out[0]);
	*eof = rd32(c.out[0] + 4);
	memcpy(to, c.out[1], length);
	return c.result;
}

static uint32_t
std_word_call(struct frpc_server *s, uint32_t std, uint32_t method, uint32_t a, uint32_t b, uint32_t c3, uint32_t words,
    uint32_t out_size, uint8_t *out)
{
	struct call c = { 0 };
	uint32_t w[3] = { a, b, c3 };

	for (uint32_t i = 0; i < words; i++) {
		prim_word(&c, w[i]);
	}
	c.out_count = out_size ? 1 : 0;
	c.out_size[0] = out_size;
	serve(s, std, method, &c);
	if (out_size) {
		memcpy(out, c.out[0], out_size);
	}
	return c.result;
}

static uint32_t
std_write(struct frpc_server *s, uint32_t std, uint32_t fd, const char *data, uint32_t *written)
{
	struct call c = { 0 };

	prim_word(&c, fd);
	prim_word(&c, (uint32_t)strlen(data));
	c.in[0] = data;
	c.in_size[0] = (uint32_t)strlen(data);
	c.in_count = 1;
	c.out_count = 1;
	c.out_size[0] = 8;
	serve(s, std, 5, &c);
	*written = rd32(c.out[0]);
	return c.result;
}

static uint32_t
std_path_call(struct frpc_server *s, uint32_t std, uint32_t method, const char *path, const char *second)
{
	struct call c = { 0 };

	if (method > 30) {
		prim_word(&c, method);
	}
	in_string(&c, path);
	if (second) {
		in_string(&c, second);
	}
	return serve(s, std, method > 30 ? 31 : method, &c);
}

static uint32_t
std_stat(struct frpc_server *s, uint32_t std, const char *path, uint8_t st[96])
{
	struct call c = { 0 };

	prim_word(&c, 31);              /* (method 31 and above: the real number in the first word) */
	in_string(&c, path);
	c.out_count = 1;
	c.out_size[0] = 96;
	serve(s, std, 31, &c);
	memcpy(st, c.out[0], 96);
	return c.result;
}

/* opendir + every readdir until the end: the names, '|'-separated */
static uint32_t
list(struct frpc_server *s, uint32_t std, const char *path, char *names, size_t size)
{
	struct call c = { 0 };
	uint64_t dir;

	names[0] = 0;
	in_string(&c, path);
	c.out_count = 1;
	c.out_size[0] = 8;
	if (serve(s, std, 26, &c) != 0) {
		return c.result;
	}
	dir = rd64(c.out[0]);
	for (int n = 0; n < 1000; n++) {
		struct call r = { 0 };
		uint32_t eof;

		prim_word64(&r, dir);
		r.out_count = 1;
		r.out_size[0] = 264;    /* DIRENT { long ino; char name[255]; } (260 with its padding), then bEOF */
		if (serve(s, std, 28, &r) != 0) {
			return r.result;
		}
		eof = rd32(r.out[0] + 260);
		if (eof) {
			CHECK(r.out[0][4] == 0, "the last readdir has an empty name");
			break;
		}
		CHECK(memchr(r.out[0] + 4, 0, 255) != NULL, "a readdir name is NUL-terminated");
		strncat(names, (const char *)r.out[0] + 4, size - strlen(names) - 2);
		strcat(names, "|");
	}
	{
		struct call x = { 0 };

		prim_word64(&x, dir);
		serve(s, std, 27, &x);
		CHECK(x.result == 0, "closedir: %u", x.result);
	}
	return 0;
}

/* ---- 1. scalars ---- */

static void
test_scalars(void)
{
	CHECK(FRPC_SC(4, 2, 2) == 0x04020200u, "next2's scalars %#x", FRPC_SC(4, 2, 2));
	CHECK(FRPC_SC(31, 2, 1) == 0x1f020100u, "an extended method %#x", FRPC_SC(31, 2, 1));
	CHECK(FRPC_SC_METHOD(0x04020200u) == 4 && FRPC_SC_INBUFS(0x04020200u) == 2 && FRPC_SC_OUTBUFS(0x04020200u) == 2,
	    "scalars read back");
	CHECK(FRPC_SC_HANDLES(0x00000012u) == 3, "handles counted");
}

/* ---- 2. the invoke payload (Linux's fastrpc_get_args) ---- */

static void
test_payload(void)
{
	static uint8_t payload[8192];
	uint8_t req[24], page[16], rsp[8] = { 0 };
	struct frpc_buf bufs[3] = { { req, sizeof(req) }, { page, sizeof(page) }, { rsp, sizeof(rsp) } };
	const uint64_t dsp = 0x580000000ull + 0x3000;   /* sid 5 in bits 32..35, the payload at IOVA 0x80003000 */
	const uint64_t pv = 0x80003000ull;
	uint32_t sc = FRPC_SC(4, 2, 1), meta = (16 + 8 + 16) * 3 + 8 * 16 + 4 * 64 + 4, size;

	for (unsigned i = 0; i < sizeof(req); i++) req[i] = (uint8_t)(0x10 + i);
	for (unsigned i = 0; i < sizeof(page); i++) page[i] = (uint8_t)(0x80 + i);
	CHECK(meta == 508, "the meta's size");
	size = frpc_payload_size(sc, bufs);
	/* args at 512 (aligned 128), 640, 768; the last ends at 776 */
	CHECK(size == 776, "payload size %u", size);
	memset(payload, 0xaa, sizeof(payload));
	CHECK(frpc_payload_build(payload, sizeof(payload), dsp, pv, sc, bufs) == size, "built");
	for (unsigned i = 0; i < 3; i++) {
		uint32_t offset = 512 + 128 * i, length = (uint32_t)bufs[i].length;
		const uint8_t *rpra = payload + 16 * i, *list = payload + 48 + 8 * i, *pages = payload + 72 + 16 * i;

		CHECK(rd64(rpra) == pv + offset, "arg %u pv %#llx", i, (unsigned long long)rd64(rpra));
		CHECK(rd64(rpra + 8) == length, "arg %u len", i);
		CHECK(rd32(list) == 1 && rd32(list + 4) == i, "arg %u list {1, %u}", i, i);
		CHECK(rd64(pages) == ((dsp + offset) & ~0xfffull), "arg %u page %#llx", i, (unsigned long long)rd64(pages));
		CHECK(rd64(pages + 8) == 4096, "arg %u page size", i);
		CHECK((rd64(rpra) & 0xfff) == ((dsp + offset) & 0xfff), "arg %u: the page offset agrees", i);
	}
	CHECK(memcmp(payload + 512, req, sizeof(req)) == 0, "in-arg 0 copied in");
	CHECK(memcmp(payload + 640, page, sizeof(page)) == 0, "in-arg 1 copied in");
	CHECK(payload[120] == 0 && payload[507] == 0, "fdlist, crclist and poll word zeroed");
	/* the DSP answers into the out-arg */
	wr64(payload + 768, 0x123456789abcull);
	frpc_payload_outputs(payload, sc, bufs);
	CHECK(rd64(rsp) == 0x123456789abcull, "out-arg copied back");

	/* an argument across a page boundary takes two pages; a zero-length one none */
	{
		static uint8_t data[5000];
		struct frpc_buf two[2] = { { data, sizeof(data) }, { NULL, 0 } };
		uint32_t sc2 = FRPC_SC(1, 2, 0), meta2 = 40 * 2 + 128 + 256 + 4;

		size = frpc_payload_size(sc2, two);
		CHECK(size == ((meta2 + 127) & ~127u) + 5000, "two args: %u", size);
		CHECK(frpc_payload_build(payload, sizeof(payload), 0x380000000ull, 0x80000000ull, sc2, two) == size, "built 2");
		/* two scalars: rpra at 0, the list at 32, the pages at 48 */
		CHECK(rd64(payload + 48 + 8) == 8192, "5000 bytes from offset 512 span two pages: %llu",
		    (unsigned long long)rd64(payload + 48 + 8));
		CHECK(rd32(payload + 32 + 8) == 0 && rd64(payload + 16 + 8) == 0, "a zero-length arg: no pages, len 0");
		CHECK(frpc_payload_build(payload, 1000, 0x380000000ull, 0x80000000ull, sc2, two) == 0, "too small a buffer: refused");
	}
}

/* ---- 3. messages ---- */

static void
test_messages(void)
{
	struct frpc_msg m;
	uint8_t raw[24] = { 0 };
	struct frpc_rsp r;

	CHECK(sizeof(struct frpc_msg) == 40, "struct fastrpc_msg is 40 bytes (%zu)", sizeof(struct frpc_msg));
	frpc_msg_make(&m, 0, 0x1d15, 0x123, 2, 1, FRPC_SC(0, 1, 0), 0x580003000ull, 776);
	CHECK(m.pid == 0 && m.tid == 0x1d15, "pid 0 for the kernel's messages");
	CHECK(m.ctx == (0x123ull << 4 | 2), "ctx: the number above the PD %#llx", (unsigned long long)m.ctx);
	CHECK(m.handle == 1 && m.sc == 0x00010000u && m.addr == 0x580003000ull && m.size == 4096, "msg fields; size a page");
	wr64(raw, m.ctx);
	wr32(raw + 8, 0xfffffff2u);
	CHECK(frpc_rsp_parse(raw, 16, &r) && r.ctx == m.ctx && r.retval == -14 && r.flags == 0, "a v1 response");
	CHECK(frpc_rsp_done(&r), "a v1 response completes");
	wr32(raw + 12, 1);              /* flags: an early response */
	wr32(raw + 20, 2);              /* version 2 */
	CHECK(frpc_rsp_parse(raw, 24, &r) && r.flags == 1 && !frpc_rsp_done(&r), "an early response does not complete");
	wr32(raw + 12, 3);
	CHECK(frpc_rsp_parse(raw, 24, &r) && frpc_rsp_done(&r), "COMPLETE_SIGNAL completes");
	CHECK(!frpc_rsp_parse(raw, 12, &r), "a short response is refused");
	CHECK(frpc_ctx_number(m.ctx) == 0x123, "the number back out of a ctx");
}

/* ---- 4. the listener's packing (listener_buf.h) ---- */

static void
test_listener_packing(void)
{
	uint8_t pack[256];
	struct frpc_view in[4];
	uint32_t out_lengths[4], at = 0;

	/* in-bufs: 8 bytes, 5 bytes, 0 bytes; then 2 out-lengths: 4 and 300 */
	memset(pack, 0, sizeof(pack));
	wr32(pack + at, 8); at += 4; at = (at + 7) & ~7u; memcpy(pack + at, "ABCDEFGH", 8); at += 8;
	wr32(pack + at, 5); at += 4; at = (at + 7) & ~7u; memcpy(pack + at, "hello", 5); at += 5;
	wr32(pack + at, 0); at += 4;
	wr32(pack + at, 4); at += 4;
	wr32(pack + at, 300); at += 4;
	CHECK(frpc_unpack_in(pack, at, FRPC_SC(19, 3, 2), in, out_lengths), "unpacked");
	CHECK(in[0].length == 8 && memcmp(in[0].data, "ABCDEFGH", 8) == 0 && in[0].data == pack + 8, "in 0");
	CHECK(in[1].length == 5 && memcmp(in[1].data, "hello", 5) == 0 && in[1].data == pack + 24, "in 1 at 24");
	CHECK(in[2].length == 0, "in 2 empty");
	CHECK(out_lengths[0] == 4 && out_lengths[1] == 300, "out lengths");
	CHECK(!frpc_unpack_in(pack, 20, FRPC_SC(19, 3, 2), in, out_lengths), "truncated: refused");
	wr32(pack + 16, 0x7fffffff);
	CHECK(!frpc_unpack_in(pack, at, FRPC_SC(19, 3, 2), in, out_lengths), "a length past the end: refused");
	CHECK(!frpc_unpack_in(pack, at, FRPC_SC(19, 40, 2), in, out_lengths), "too many buffers: refused");

	/*
	 * the answer (listener_buf.h pack_out_bufs: a length, then, if not empty, padding to 8 and the bytes):
	 * lengths 8, 3, 0, 9 -> len@0 data@8..16 | len@16 data@24..27 | len@27 | len@31 data@40..49
	 */
	{
		uint32_t lengths[4] = { 8, 3, 0, 9 };
		struct frpc_buf out[4];
		uint8_t answer[128];
		uint32_t size = frpc_pack_out_size(4, lengths);

		CHECK(size == 49, "packed size %u", size);
		memset(answer, 0xcc, sizeof(answer));
		CHECK(frpc_pack_out_layout(answer, sizeof(answer), 4, lengths, out) == size, "laid out");
		CHECK(rd32(answer) == 8 && out[0].data == answer + 8 && out[0].length == 8, "out 0");
		CHECK(rd32(answer + 16) == 3 && out[1].data == answer + 24 && out[1].length == 3, "out 1 aligned to 24");
		CHECK(rd32(answer + 27) == 0 && out[2].length == 0, "out 2's length at 27 (unaligned), no bytes");
		CHECK(rd32(answer + 31) == 9 && out[3].data == answer + 40, "out 3 aligned to 40");
		CHECK(answer[4] == 0 && answer[20] == 0 && answer[39] == 0 && answer[48] == 0, "padding and bytes zeroed");
		CHECK(frpc_pack_out_layout(answer, size - 1, 4, lengths, out) == 0, "too small: refused");
	}
}

/* ---- 5. the archive ---- */

static void
test_archive(void)
{
	struct frpc_server *s = calloc(1, sizeof(*s));
	uint32_t size;
	uint8_t *a = make_archive(tree, TREE, &size);

	CHECK(frpc_server_init(s, &ops, a, size), "a good archive");
	CHECK(s->count == TREE, "its files");
	memcpy(a, "DLSNSFS0", 8);
	CHECK(!frpc_server_init(s, &ops, a, size), "a wrong magic: refused");
	memcpy(a, "DLSNSFS1", 8);
	wr32(a + 24, size);             /* entry 0's data past the end */
	CHECK(!frpc_server_init(s, &ops, a, size), "data past the end: refused");
	free(a);
	a = make_archive(tree, TREE, &size);
	wr32(a + 16, size - 3);         /* entry 0's path past the end */
	CHECK(!frpc_server_init(s, &ops, a, size), "a path past the end: refused");
	free(a);
	{
		struct file bad[1] = { { "relative/path", "x", 1 } };

		a = make_archive(bad, 1, &size);
		CHECK(!frpc_server_init(s, &ops, a, size), "a relative path: refused");
		free(a);
	}
	CHECK(!frpc_server_init(s, &ops, NULL, 0), "no archive: refused");
	free(s);
}

/* ---- 6. the server ---- */

static struct frpc_server *
new_server(uint8_t **archive)
{
	struct frpc_server *s = calloc(1, sizeof(*s));
	uint32_t size;

	*archive = make_archive(tree, TREE, &size);
	frpc_server_init(s, &ops, *archive, size);
	return s;
}

static void
test_remotectl(void)
{
	uint8_t *a;
	struct frpc_server *s = new_server(&a);
	uint32_t handle;

	CHECK(open_interface(s, "apps_std", &handle) == 0 && handle == FRPC_HANDLE_APPS_STD, "apps_std -> %u", handle);
	CHECK(open_interface(s, "apps_mem", &handle) == 0 && handle == FRPC_HANDLE_APPS_MEM, "apps_mem -> %u", handle);
	CHECK(open_interface(s, "nonexistent_iface", &handle) != 0, "an unknown interface is refused");
	{
		struct call c = { 0 };

		prim_word(&c, FRPC_HANDLE_APPS_STD);
		prim_word(&c, 32);
		c.out_count = 2;
		c.out_size[0] = 4;
		c.out_size[1] = 32;
		CHECK(serve(s, 0, 1, &c) == 0 && rd32(c.out[0]) == 0 && c.out[1][0] == 0, "close");
	}
	{
		struct call c = { 0 };

		CHECK(serve(s, 9, 0, &c) == FRPC_AEE_EUNSUPPORTED, "an unknown handle");
		CHECK(serve(s, 0, 7, &c) == FRPC_AEE_EUNSUPPORTED, "an unknown method");
	}
	{
		struct frpc_view in[1] = { { (const uint8_t *)"\0\0\0\0", 4 } };
		struct frpc_buf out[1] = { { NULL, 0 } };

		CHECK(frpc_server_call(s, 0, FRPC_SC(0, 1, 0) | 0x10, in, out) == FRPC_AEE_EBADPARM, "handles in sc: refused");
		CHECK(frpc_server_call(s, 0, FRPC_SC(0, 1, 0), in, out) == FRPC_AEE_EBADPARM, "too few buffers: refused");
	}
	free(s);
	free(a);
}

static void
test_read(void)
{
	uint8_t *a, buf[512], st[96];
	struct frpc_server *s = new_server(&a);
	uint32_t std = FRPC_HANDLE_APPS_STD, fd, got, eof;
	uint8_t word[8];

	CHECK(std_open(s, std, "/vendor/etc/sensors/sns_reg_config", "r", &fd) == 0, "open sns_reg_config");
	CHECK(std_read(s, std, fd, 4, buf, &got, &eof) == 0 && got == 4 && eof == 0 && memcmp(buf, "vers", 4) == 0, "read 4");
	CHECK(std_read(s, std, fd, 100, buf, &got, &eof) == 0 && got == 6 && eof == 1 && memcmp(buf, "ion=6\n", 6) == 0, "the rest");
	CHECK(std_word_call(s, std, 8, fd, 0, 0, 1, 4, word) == 0 && rd32(word) == 10, "ftell 10");
	CHECK(std_word_call(s, std, 9, fd, 2, 0, 3, 0, NULL) == 0, "fseek SET 2");
	CHECK(std_read(s, std, fd, 3, buf, &got, &eof) == 0 && got == 3 && memcmp(buf, "rsi", 3) == 0, "read after seek");
	CHECK(std_word_call(s, std, 9, fd, (uint32_t)-1, 2, 3, 0, NULL) == 0, "fseek END -1");
	CHECK(std_read(s, std, fd, 3, buf, &got, &eof) == 0 && got == 1 && buf[0] == '\n', "the last byte");
	CHECK(std_word_call(s, std, 9, fd, (uint32_t)-100, 1, 3, 0, NULL) != 0, "fseek before the start: refused");
	CHECK(std_word_call(s, std, 10, fd, 0, 0, 1, 8, word) == 0 && rd64(word) == 10, "flen 10");
	CHECK(std_word_call(s, std, 11, fd, 0, 0, 1, 0, NULL) == 0, "rewind");
	CHECK(std_word_call(s, std, 12, fd, 0, 0, 1, 4, word) == 0 && rd32(word) == 0, "feof 0 after rewind");
	CHECK(std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL) == 0, "fclose");
	CHECK(std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL) != 0, "fclose twice: refused");
	CHECK(std_read(s, std, fd, 4, buf, &got, &eof) != 0, "read of a closed file: refused");

	/* the aliases Android's paths take */
	CHECK(std_open(s, std, "/system/vendor/etc/sensors/sns_reg_config", "r", &fd) == 0, "/system/vendor alias");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(std_open(s, std, "/persist/sensors/registry/registry/stk3a6x_0", "rb", &fd) == 0, "/persist alias");
	CHECK(std_read(s, std, fd, 7, buf, &got, &eof) == 0 && got == 7 && memcmp(buf, "{\"b\":2}", 7) == 0, "its bytes");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(std_open(s, std, "//vendor/./etc/sensors//sns_reg_config", "r", &fd) == 0, "doubled slashes and dots");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(std_open(s, std, "/vendor/etc/sensors/../sensors/sns_reg_config", "r", &fd) != 0, "'..' is refused");
	CHECK(std_open(s, std, "/vendor/etc/sensors/nonexistent", "r", &fd) == FRPC_ENOENT, "missing: ENOENT");
	CHECK(std_open(s, std, "/vendor/etc/sensors", "r", &fd) != 0, "a directory is not a file");

	/* relative names go through the library path (";/vendor/dsp/adsp;...") */
	{
		struct call c = { 0 };

		in_string(&c, "ADSP_LIBRARY_PATH");
		in_string(&c, ";");
		in_string(&c, "soc_id");
		in_string(&c, "r");
		c.out_count = 1;
		c.out_size[0] = 4;
		CHECK(serve(s, std, 19, &c) == FRPC_ENOENT, "a relative name not on the library path");
	}

	/* fopen (method 0) */
	{
		struct call c = { 0 };

		in_string(&c, "/sys/devices/soc0/soc_id");
		in_string(&c, "r");
		c.out_count = 1;
		c.out_size[0] = 4;
		CHECK(serve(s, std, 0, &c) == 0, "fopen");
		fd = rd32(c.out[0]);
		CHECK(std_read(s, std, fd, 64, buf, &got, &eof) == 0 && got == 4 && memcmp(buf, "475\n", 4) == 0, "soc_id");
		std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	}

	/* fgets */
	{
		struct call c = { 0 };

		std_open(s, std, "/vendor/etc/sensors/sns_reg_config", "r", &fd);
		prim_word(&c, fd);
		prim_word(&c, 64);
		c.out_count = 2;
		c.out_size[0] = 4;
		c.out_size[1] = 64;
		CHECK(serve(s, std, 20, &c) == 0 && strcmp((char *)c.out[1], "version=6\n") == 0 && rd32(c.out[0]) == 0, "fgets");
		std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	}

	/* a large file in pieces */
	{
		uint32_t total = 0;

		CHECK(std_open(s, std, "/vendor/etc/sensors/config/sm7350_icm4x6xx_0.json", "r", &fd) == 0, "the large file");
		for (int i = 0; i < 20; i++) {
			CHECK(std_read(s, std, fd, 512, buf, &got, &eof) == 0, "read");
			CHECK(memcmp(buf, big + total, got) == 0, "its bytes at %u", total);
			total += got;
			if (eof) {
				break;
			}
		}
		CHECK(total == sizeof(big), "all %u bytes", total);
		std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	}

	/* stat */
	CHECK(std_stat(s, std, "/vendor/etc/sensors/sns_reg_config", st) == 0, "stat a file");
	CHECK((rd32(st + 24) & 0170000) == 0100000 && rd64(st + 40) == 10, "a regular file of 10 bytes (mode %o)", rd32(st + 24));
	CHECK(std_stat(s, std, "/vendor/etc/sensors/config", st) == 0 && (rd32(st + 24) & 0170000) == 0040000, "stat a directory");
	CHECK(std_stat(s, std, "/vendor/etc/nonexistent", st) == FRPC_ENOENT, "stat a missing name");

	/* fileExists */
	{
		struct call c = { 0 };

		in_string(&c, "/mnt/vendor/persist/sensors/registry/sns_reg_version");
		c.out_count = 1;
		c.out_size[0] = 1;
		CHECK(serve(s, std, 22, &c) == 0 && c.out[0][0] == 1, "fileExists yes");
		c = (struct call){ 0 };
		in_string(&c, "/mnt/vendor/persist/nonexistent");
		c.out_count = 1;
		c.out_size[0] = 1;
		CHECK(serve(s, std, 22, &c) == 0 && c.out[0][0] == 0, "fileExists no");
	}
	free(s);
	free(a);
}

static void
test_directories(void)
{
	uint8_t *a;
	struct frpc_server *s = new_server(&a);
	uint32_t std = FRPC_HANDLE_APPS_STD;
	char names[1024];

	CHECK(list(s, std, "/mnt/vendor/persist/sensors/registry/registry", names, sizeof(names)) == 0, "list the registry");
	CHECK(strcmp(names, ".|..|icm4x6xx_0|stk3a6x_0|") == 0, "the registry's names: %s", names);
	CHECK(list(s, std, "/vendor/etc/sensors/", names, sizeof(names)) == 0, "a trailing slash");
	CHECK(strcmp(names, ".|..|config|sns_reg_config|") == 0, "a directory listed once: %s", names);
	CHECK(list(s, std, "/", names, sizeof(names)) == 0 && strcmp(names, ".|..|mnt|sys|vendor|") == 0, "the root: %s", names);
	CHECK(list(s, std, "/vendor/etc/sensors/sns_reg_config", names, sizeof(names)) != 0, "a file is not a directory");
	CHECK(list(s, std, "/nonexistent", names, sizeof(names)) == FRPC_ENOENT, "a missing directory");
	free(s);
	free(a);
}

static void
test_overlay(void)
{
	uint8_t *a, buf[512];
	struct frpc_server *s = new_server(&a);
	uint32_t std = FRPC_HANDLE_APPS_STD, fd, got, eof, written;
	char names[1024];
	const char *reg = "/mnt/vendor/persist/sensors/registry/registry";
	char temp[128], final[128], base[128];

	snprintf(temp, sizeof(temp), "%s/temp.json", reg);
	snprintf(final, sizeof(final), "%s/accel_new", reg);
	snprintf(base, sizeof(base), "%s/icm4x6xx_0", reg);

	CHECK(std_open(s, std, temp, "w", &fd) == 0, "create in persist (in RAM)");
	CHECK(std_write(s, std, fd, "{\"x\":", &written) == 0 && written == 5, "write 1");
	CHECK(std_write(s, std, fd, "42}", &written) == 0 && written == 3, "write 2");
	CHECK(std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL) == 0, "close");
	CHECK(std_open(s, std, temp, "r", &fd) == 0, "read it back");
	CHECK(std_read(s, std, fd, 100, buf, &got, &eof) == 0 && got == 8 && memcmp(buf, "{\"x\":42}", 8) == 0, "its bytes");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(std_path_call(s, std, 33, temp, final) == 0, "frename");
	CHECK(std_open(s, std, temp, "r", &fd) == FRPC_ENOENT, "the old name is gone");
	CHECK(std_open(s, std, final, "r", &fd) == 0, "the new one is there");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(list(s, std, reg, names, sizeof(names)) == 0 && strcmp(names, ".|..|icm4x6xx_0|stk3a6x_0|accel_new|") == 0,
	    "listed: %s", names);

	/* a base file written over: the overlay shadows it, the archive is untouched */
	CHECK(std_open(s, std, base, "w", &fd) == 0, "truncate a base file");
	CHECK(std_write(s, std, fd, "new", &written) == 0, "write");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(std_open(s, std, base, "r", &fd) == 0 && std_read(s, std, fd, 100, buf, &got, &eof) == 0 && got == 3 &&
	    memcmp(buf, "new", 3) == 0, "the new bytes win");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(memcmp(a + 16 + 16 * TREE, tree[0].path, strlen(tree[0].path)) == 0, "the archive is untouched");
	/* append */
	CHECK(std_open(s, std, base, "a", &fd) == 0 && std_write(s, std, fd, "+1", &written) == 0, "append");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(std_open(s, std, base, "r", &fd) == 0 && std_read(s, std, fd, 100, buf, &got, &eof) == 0 && got == 5 &&
	    memcmp(buf, "new+1", 5) == 0, "appended");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	/* r+ on a base file copies it first */
	{
		char other[128];

		snprintf(other, sizeof(other), "%s/stk3a6x_0", reg);
		CHECK(std_open(s, std, other, "r+", &fd) == 0 && std_write(s, std, fd, "[", &written) == 0, "r+ write");
		std_word_call(s, std, 9, fd, 0, 0, 3, 0, NULL);
		CHECK(std_read(s, std, fd, 100, buf, &got, &eof) == 0 && got == 7 && memcmp(buf, "[\"b\":2}", 7) == 0,
		    "the copy changed in place");
		std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	}
	/* remove a base file */
	CHECK(std_path_call(s, std, 24, base, NULL) == 0, "fremove");
	CHECK(std_open(s, std, base, "r", &fd) == FRPC_ENOENT, "removed");
	CHECK(std_path_call(s, std, 24, base, NULL) == FRPC_ENOENT, "removed twice");
	CHECK(list(s, std, reg, names, sizeof(names)) == 0 && strcmp(names, ".|..|stk3a6x_0|accel_new|") == 0, "listed: %s", names);
	/* re-create after remove */
	CHECK(std_open(s, std, base, "w", &fd) == 0 && std_write(s, std, fd, "again", &written) == 0, "re-create");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	CHECK(list(s, std, reg, names, sizeof(names)) == 0 && strcmp(names, ".|..|icm4x6xx_0|stk3a6x_0|accel_new|") == 0,
	    "listed: %s", names);

	/* writes outside persist and the sensors' data directory are refused */
	CHECK(std_open(s, std, "/vendor/etc/sensors/sns_reg_config", "w", &fd) == FRPC_EACCES, "no writes to /vendor");
	CHECK(std_open(s, std, "/vendor/etc/sensors/sns_reg_config", "r+", &fd) == FRPC_EACCES, "no r+ on /vendor");
	CHECK(std_path_call(s, std, 24, "/vendor/etc/sensors/sns_reg_config", NULL) == FRPC_EACCES, "no remove on /vendor");
	CHECK(std_path_call(s, std, 33, "/vendor/etc/sensors/sns_reg_config", final) == FRPC_EACCES, "no rename from /vendor");
	CHECK(std_open(s, std, "/data/vendor/sensors/sensors_list.txt", "w", &fd) == 0, "the sensors' data directory is writable");
	std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	/* mkdir in persist */
	{
		struct call c = { 0 };

		in_string(&c, "/mnt/vendor/persist/sensors/newdir");
		prim_word(&c, 0770);
		CHECK(serve(s, std, 29, &c) == 0, "mkdir");
		CHECK(list(s, std, "/mnt/vendor/persist/sensors", names, sizeof(names)) == 0 &&
		    strcmp(names, ".|..|registry|newdir|") == 0, "the new directory: %s", names);
	}
	/* the overlay's size is bounded */
	{
		static char chunk[60000];

		memset(chunk, 'z', sizeof(chunk) - 1);
		CHECK(std_open(s, std, temp, "w", &fd) == 0, "a big one");
		for (int i = 0; i < 100 && std_write(s, std, fd, chunk, &written) == 0; i++) {
		}
		CHECK(s->overlay_bytes <= FRPC_OVERLAY_MAX, "bounded: %u", s->overlay_bytes);
		std_word_call(s, std, 3, fd, 0, 0, 1, 0, NULL);
	}
	frpc_server_reset(s);
	CHECK(allocated == 0, "everything allocated is given back (%u left)", allocated);
	free(s);
	free(a);
}

static void
test_misc(void)
{
	uint8_t *a;
	struct frpc_server *s = new_server(&a);
	uint32_t std = FRPC_HANDLE_APPS_STD, fd;

	/* getenv */
	{
		struct call c = { 0 };

		in_string(&c, "ADSP_LIBRARY_PATH");
		prim_word(&c, 128);
		c.out_count = 2;
		c.out_size[0] = 4;
		c.out_size[1] = 128;
		CHECK(serve(s, std, 16, &c) == 0 && c.out[1][0] == ';' && rd32(c.out[0]) == strlen((char *)c.out[1]) + 1,
		    "ADSP_LIBRARY_PATH: %s", (char *)c.out[1]);
		c = (struct call){ 0 };
		in_string(&c, "NOT_SET");
		prim_word(&c, 16);
		c.out_count = 2;
		c.out_size[0] = 4;
		c.out_size[1] = 16;
		CHECK(serve(s, std, 16, &c) != 0, "an unset variable");
	}
	/* print_string */
	logs = 0;
	CHECK(std_path_call(s, std, 15, "hello from the DSP", NULL) == 0 && logs == 1 && strstr(last_log, "hello from the DSP"),
	    "print_string logged: %s", last_log);
	/* fflush, setenv */
	std_open(s, std, "/sys/devices/soc0/soc_id", "r", &fd);
	CHECK(std_word_call(s, std, 2, fd, 0, 0, 1, 0, NULL) == 0, "fflush");
	CHECK(std_word_call(s, std, 23, fd, 0, 0, 1, 0, NULL) == 0, "fsync");
	/* the file table runs out, and comes back */
	{
		uint32_t opened = 0, fds[FRPC_FILES + 4];

		for (uint32_t i = 0; i < FRPC_FILES + 4; i++) {
			if (std_open(s, std, "/sys/devices/soc0/soc_id", "r", &fds[opened]) == 0) {
				opened++;
			}
		}
		CHECK(opened == FRPC_FILES - 1, "%u files open at most (one was open already)", opened);
		for (uint32_t i = 0; i < opened; i++) {
			std_word_call(s, std, 3, fds[i], 0, 0, 1, 0, NULL);
		}
		CHECK(std_open(s, std, "/sys/devices/soc0/soc_id", "r", &fds[0]) == 0, "free again");
	}
	/* an unsupported apps_std method */
	{
		struct call c = { 0 };

		prim_word(&c, fd);
		CHECK(serve(s, std, 25, &c) == FRPC_AEE_EUNSUPPORTED, "fdopen_decrypt: unsupported");
	}
	/* apps_mem: request_map64 asks the kernel for pages and answers the DSP's address */
	{
		struct call c = { 0 };

		prim_word(&c, 0);               /* heap id */
		prim_word(&c, 0);               /* ion flags */
		prim_word(&c, 0x1000);          /* ADSP_MMAP_ADD_PAGES */
		prim_word(&c, 0);               /* (padding before the 64-bit ones) */
		prim_word64(&c, 0);
		prim_word64(&c, 0x100000);
		c.out_count = 1;
		c.out_size[0] = 16;
		CHECK(serve(s, FRPC_HANDLE_APPS_MEM, 2, &c) == 0, "request_map64");
		CHECK(mapped_length == 0x100000 && mapped_flags == 0x1000, "the kernel was asked for 1 MB");
		CHECK(rd64(c.out[0]) == 0 && rd64(c.out[0] + 8) == 0xe0001000ull, "vapps 0, vadsp from the DSP");
		c = (struct call){ 0 };
		prim_word(&c, 0);
		prim_word(&c, 0);
		prim_word(&c, 4);               /* ADSP_MMAP_HEAP_ADDR: not done (hexagonrpcd neither) */
		prim_word(&c, 0);
		prim_word64(&c, 0);
		prim_word64(&c, 0x1000);
		c.out_count = 1;
		c.out_size[0] = 16;
		CHECK(serve(s, FRPC_HANDLE_APPS_MEM, 2, &c) == FRPC_AEE_EUNSUPPORTED, "other kinds of mapping: unsupported");
	}
	/* a short primary buffer is refused, not read past */
	{
		struct frpc_view in[1] = { { (const uint8_t *)"\1\0", 2 } };
		struct frpc_buf out[2] = { { NULL, 0 }, { NULL, 0 } };

		CHECK(frpc_server_call(s, std, FRPC_SC(4, 1, 2), in, out) == FRPC_AEE_EBADPARM, "a short prim: refused");
	}
	/* a string that is not NUL-terminated is refused */
	{
		struct call c = { 0 };
		char unterminated[4] = { 'a', 'b', 'c', 'd' };

		prim_word(&c, 4);
		c.in[0] = unterminated;
		c.in_size[0] = 4;
		c.in_count = 1;
		prim_word(&c, 2);
		c.in[1] = "r";
		c.in_size[1] = 2;
		c.in_count = 2;
		c.out_count = 1;
		c.out_size[0] = 4;
		CHECK(serve(s, std, 0, &c) == FRPC_AEE_EBADPARM, "unterminated: refused");
	}
	frpc_server_reset(s);
	free(s);
	free(a);
}

/* ---- 7. a whole conversation: what the listener thread does with next2's answer ---- */

static void
test_round_trip(void)
{
	uint8_t *a;
	struct frpc_server *s = new_server(&a);
	uint8_t pack[512], answer[512];
	struct frpc_view in[FRPC_MAX_BUFS];
	struct frpc_buf out[FRPC_MAX_BUFS];
	uint32_t lengths[FRPC_MAX_BUFS], at = 0, size, sc = FRPC_SC(0, 2, 2);
	const char *name = "apps_std";

	/* the DSP's remotectl open("apps_std"), as next2 delivers it */
	wr32(pack + at, 8); at += 4; at = (at + 7) & ~7u;
	wr32(pack + at, (uint32_t)strlen(name) + 1); wr32(pack + at + 4, 32); at += 8;
	wr32(pack + at, (uint32_t)strlen(name) + 1); at += 4; at = (at + 7) & ~7u;
	memcpy(pack + at, name, strlen(name) + 1); at += (uint32_t)strlen(name) + 1;
	wr32(pack + at, 8); at += 4;
	wr32(pack + at, 32); at += 4;
	CHECK(frpc_unpack_in(pack, at, sc, in, lengths), "unpacked");
	size = frpc_pack_out_layout(answer, sizeof(answer), FRPC_SC_OUTBUFS(sc), lengths, out);
	CHECK(size == 56, "laid out %u", size);         /* len@0 data@8..16, len@16 data@24..56 */
	CHECK(frpc_server_call(s, 0, sc, in, out) == 0, "served");
	CHECK(rd32(answer) == 8 && rd32(answer + 8) == FRPC_HANDLE_APPS_STD && rd32(answer + 12) == 0, "the handle in the answer");
	CHECK(rd32(answer + 16) == 32 && answer[24] == 0, "dlerror empty");
	free(s);
	free(a);
}

int
main(void)
{
	for (unsigned i = 0; i < sizeof(big); i++) {
		big[i] = (char)('a' + i % 26);
	}
	test_scalars();
	test_payload();
	test_messages();
	test_listener_packing();
	test_archive();
	test_remotectl();
	test_read();
	test_directories();
	test_overlay();
	test_misc();
	test_round_trip();
	printf("fastrpc-test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
