/*
 * darwin-lisa: FastRPC as the sensors PD on lisa's audio DSP speaks it, and the file server it reads its configuration
 * through: the pure part of dlisa.fastrpc.c, kept free of the kernel so that test/fastrpc-test.c runs it on the host.
 * Whoever includes this provides memcpy, memset, memcmp, strlen, strcmp, strncmp, strchr and snprintf (the kernel's libkern,
 * or the host's libc), and <stdint.h>/<stdbool.h>.
 *
 * Where each format comes from (kernel/drivers/adsp/README.md, "Sensors: FastRPC"):
 *   - the invoke payload, the message and the response: Linux's drivers/misc/fastrpc.c (fastrpc_get_args(),
 *     struct fastrpc_msg, struct fastrpc_invoke_rsp) and the vendor's adsprpc.c (struct smq_invoke_rspv2);
 *   - the reverse calls' packing: Qualcomm's fastrpc listener_buf.h (pack_out_bufs(), unpack_in_bufs());
 *   - the methods and their primary buffers: the IDLs (remotectl.idl, apps_std.idl, apps_mem.idl, adspmsgd_apps.idl)
 *     as QAIC lays them out, checked against hexagonrpcd's tables (linux-msm/hexagonrpc).
 *
 * The file server: a read-only tree (an archive built by fw-sensors-stage.py and linked into the kernel: the vendor's
 * etc/sensors, the registry read from the phone's persist partition, socinfo) under an overlay in memory that takes the
 * DSP's writes. Nothing written ever reaches a disk: persist is never written.
 */
#ifndef DLISA_FASTRPC_PROTO_H
#define DLISA_FASTRPC_PROTO_H

/* ---- scalars (remote.h REMOTE_SCALARS_MAKEX) ---------------------------------------------------------------- */

#define FRPC_SC(method, in, out)        ((uint32_t)(((uint32_t)(method) & 0x1f) << 24 | ((uint32_t)(in) & 0xff) << 16 | \
	                                 ((uint32_t)(out) & 0xff) << 8))
#define FRPC_SC_METHOD(sc)              (((sc) >> 24) & 0x1f)
#define FRPC_SC_INBUFS(sc)              (((sc) >> 16) & 0xff)
#define FRPC_SC_OUTBUFS(sc)             (((sc) >> 8) & 0xff)
#define FRPC_SC_HANDLES(sc)             ((((sc) >> 4) & 0xf) + ((sc) & 0xf))
#define FRPC_METHOD_EXTENDED            31      /* methods from 31 on: the real number is the primary buffer's first word */

/* results: the DSP's own (AEEStdErr.h, on the application processors' side, offset 0), or an errno as Android's libc gives */
#define FRPC_AEE_EFAILED                1u
#define FRPC_AEE_ENOMEMORY              2u
#define FRPC_AEE_EBADPARM               14u
#define FRPC_AEE_EUNSUPPORTED           20u
#define FRPC_ENOENT                     2u
#define FRPC_EBADF                      9u
#define FRPC_EACCES                     13u
#define FRPC_ENOTDIR                    20u
#define FRPC_EISDIR                     21u
#define FRPC_EINVAL                     22u
#define FRPC_EMFILE                     24u
#define FRPC_ENOSPC                     28u
#define FRPC_ENAMETOOLONG               36u
#define FRPC_EUNSET                     0xffffffffu     /* Android's getenv of a variable nobody set: ERRNO with errno 0 */

#define FRPC_ALIGN                      128u    /* each inline argument of an invoke starts on 128 bytes (FASTRPC_ALIGN) */
#define FRPC_PAGE                       4096u   /* the DSP's view: Linux's PAGE_SIZE on Android */
#define FRPC_MAX_BUFS                   16u     /* buffers per call; the IDLs here need 5 at most */
#define FRPC_MAX_ARG                    (16u << 20)

struct frpc_buf {
	void *data;
	uint32_t length;
};

struct frpc_view {
	const uint8_t *data;
	uint32_t length;
};

static inline uint32_t
frpc_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
frpc_rd64(const uint8_t *p)
{
	return (uint64_t)frpc_rd32(p) | (uint64_t)frpc_rd32(p + 4) << 32;
}

static inline void
frpc_wr32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static inline void
frpc_wr64(uint8_t *p, uint64_t v)
{
	frpc_wr32(p, (uint32_t)v);
	frpc_wr32(p + 4, (uint32_t)(v >> 32));
}

static inline uint32_t
frpc_align(uint32_t at, uint32_t to)
{
	return (at + to - 1) & ~(to - 1);
}

/* ---- the invoke payload: Linux's fastrpc_get_args() -------------------------------------------------------- */

/*
 * [remote args: {u64 pv, u64 len} per scalar][invoke bufs: {u32 num, u32 pgidx}][phy pages: {u64 addr, u64 size}]
 * [fdlist: 16 x u64][crclist: 64 x u32][poll word: u32], then each non-empty argument inline from a 128-byte boundary.
 */
static inline uint32_t
frpc_meta_size(uint32_t scalars)
{
	return (16u + 8u + 16u) * scalars + 8u * 16u + 4u * 64u + 4u;
}

static inline uint32_t
frpc_payload_size(uint32_t sc, const struct frpc_buf *bufs)
{
	uint32_t n = FRPC_SC_INBUFS(sc) + FRPC_SC_OUTBUFS(sc), size = frpc_meta_size(n);

	if (n > FRPC_MAX_BUFS || FRPC_SC_HANDLES(sc) != 0) {
		return 0;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (bufs[i].length > FRPC_MAX_ARG) {
			return 0;
		}
		size = bufs[i].length == 0 ? size : frpc_align(size, FRPC_ALIGN) + bufs[i].length;
	}
	return size;
}

/*
 * The payload at `payload` (CPU) that the DSP knows at `dsp` (its address: the SMMU's input address, with the context
 * bank's number in bits 32 and up, as Linux's fastrpc_buf_alloc() makes it), and whose IOVA without those bits is `pv`.
 * Each argument's pv keeps the page offset of its pages (the DSP finds the bytes at the page's mapping plus pv's offset).
 * The size laid out, or 0 when it does not fit in `capacity`.
 */
static inline uint32_t
frpc_payload_build(uint8_t *payload, uint32_t capacity, uint64_t dsp, uint64_t pv, uint32_t sc, const struct frpc_buf *bufs)
{
	uint32_t n = FRPC_SC_INBUFS(sc) + FRPC_SC_OUTBUFS(sc), size = frpc_payload_size(sc, bufs), at = frpc_meta_size(n);

	if (size == 0 || size > capacity) {
		return 0;
	}
	memset(payload, 0, size);
	for (uint32_t i = 0; i < n; i++) {
		uint8_t *rpra = payload + 16 * i, *list = payload + 16 * n + 8 * i, *page = payload + 24 * n + 16 * i;
		uint64_t first, last;

		frpc_wr64(rpra + 8, bufs[i].length);
		frpc_wr32(list + 4, i);
		if (bufs[i].length == 0) {
			continue;
		}
		at = frpc_align(at, FRPC_ALIGN);
		first = (dsp + at) & ~(uint64_t)(FRPC_PAGE - 1);
		last = (dsp + at + bufs[i].length - 1) & ~(uint64_t)(FRPC_PAGE - 1);
		frpc_wr64(rpra, pv + at);
		frpc_wr32(list, 1);
		frpc_wr64(page, first);
		frpc_wr64(page + 8, last - first + FRPC_PAGE);
		if (i < FRPC_SC_INBUFS(sc)) {
			memcpy(payload + at, bufs[i].data, bufs[i].length);
		}
		at += bufs[i].length;
	}
	return size;
}

/* The out-arguments, copied back from the payload the DSP has answered into (Linux's fastrpc_put_args()). */
static inline void
frpc_payload_outputs(const uint8_t *payload, uint32_t sc, struct frpc_buf *bufs)
{
	uint32_t n = FRPC_SC_INBUFS(sc) + FRPC_SC_OUTBUFS(sc), at = frpc_meta_size(n);

	for (uint32_t i = 0; i < n; i++) {
		uint64_t said = frpc_rd64(payload + 16 * i + 8);

		if (bufs[i].length == 0) {
			continue;
		}
		at = frpc_align(at, FRPC_ALIGN);
		if (i >= FRPC_SC_INBUFS(sc)) {
			memcpy(bufs[i].data, payload + at, said < bufs[i].length ? (uint32_t)said : bufs[i].length);
		}
		at += bufs[i].length;
	}
}

/* ---- the message and its response ---------------------------------------------------------------------------- */

struct frpc_msg {
	int32_t pid;                    /* the client's process; 0 for the kernel's own messages (attach, mmap) */
	int32_t tid;
	uint64_t ctx;                   /* ours, echoed; its low 4 bits: the PD (2: the sensors PD) */
	uint32_t handle;
	uint32_t sc;
	uint64_t addr;                  /* the payload, as the DSP knows it */
	uint64_t size;                  /* in whole pages */
};

struct frpc_rsp {
	uint64_t ctx;
	int32_t retval;
	uint32_t flags;                 /* v2 (smq_invoke_rspv2): 0 normal, 1 early, 2 user early signal, 3 complete */
	uint32_t early_wake;
	uint32_t version;
};

static inline void
frpc_msg_make(struct frpc_msg *m, int32_t pid, int32_t tid, uint32_t number, uint32_t pd, uint32_t handle, uint32_t sc,
    uint64_t addr, uint32_t payload_size)
{
	m->pid = pid;
	m->tid = tid;
	m->ctx = (uint64_t)number << 4 | (pd & 0xf);
	m->handle = handle;
	m->sc = sc;
	m->addr = addr;
	m->size = frpc_align(payload_size, FRPC_PAGE);
}

static inline uint32_t
frpc_ctx_number(uint64_t ctx)
{
	return (uint32_t)(ctx >> 4);
}

static inline bool
frpc_rsp_parse(const uint8_t *data, uint32_t length, struct frpc_rsp *r)
{
	if (length < 16) {              /* sizeof(struct fastrpc_invoke_rsp): u64 ctx, int retval, padding */
		return false;
	}
	memset(r, 0, sizeof(*r));
	r->ctx = frpc_rd64(data);
	r->retval = (int32_t)frpc_rd32(data + 8);
	if (length >= 24) {
		r->flags = frpc_rd32(data + 12);
		r->early_wake = frpc_rd32(data + 16);
		r->version = frpc_rd32(data + 20);
	}
	return true;
}

/* Early responses only announce the end (the vendor's context_notify_user()); everything else ends the call. */
static inline bool
frpc_rsp_done(const struct frpc_rsp *r)
{
	return r->flags != 1 && r->flags != 2;
}

/* ---- the listener's packing (listener_buf.h) ------------------------------------------------------------------ */

/* next2's bufs: each in-buffer as {u32 length, padding to 8 if not empty, bytes}, then the out-buffers' u32 lengths. */
static inline bool
frpc_unpack_in(const uint8_t *pack, uint32_t length, uint32_t sc, struct frpc_view *in, uint32_t *out_lengths)
{
	uint32_t at = 0, ins = FRPC_SC_INBUFS(sc), outs = FRPC_SC_OUTBUFS(sc);

	if (ins > FRPC_MAX_BUFS || outs > FRPC_MAX_BUFS) {
		return false;
	}
	for (uint32_t i = 0; i < ins; i++) {
		uint32_t size;

		if (at > length || length - at < 4) {
			return false;
		}
		size = frpc_rd32(pack + at);
		at += 4;
		in[i].data = pack + at;
		in[i].length = size;
		if (size == 0) {
			continue;
		}
		at = frpc_align(at, 8);
		if (at > length || size > length - at) {
			return false;
		}
		in[i].data = pack + at;
		at += size;
	}
	for (uint32_t i = 0; i < outs; i++) {
		if (at > length || length - at < 4) {
			return false;
		}
		out_lengths[i] = frpc_rd32(pack + at);
		at += 4;
		if (out_lengths[i] > FRPC_MAX_ARG) {
			return false;
		}
	}
	return true;
}

static inline uint32_t
frpc_pack_out_size(uint32_t n, const uint32_t *lengths)
{
	uint32_t at = 0;

	for (uint32_t i = 0; i < n; i++) {
		at += 4;
		at = lengths[i] == 0 ? at : frpc_align(at, 8) + lengths[i];
	}
	return at;
}

/* The answer laid out, zeroed, with its lengths written: out[i] points where buffer i's bytes go. Its size, or 0. */
static inline uint32_t
frpc_pack_out_layout(uint8_t *pack, uint32_t capacity, uint32_t n, const uint32_t *lengths, struct frpc_buf *out)
{
	uint32_t size = frpc_pack_out_size(n, lengths), at = 0;

	if (n > FRPC_MAX_BUFS || size > capacity) {
		return 0;
	}
	memset(pack, 0, size);
	for (uint32_t i = 0; i < n; i++) {
		frpc_wr32(pack + at, lengths[i]);
		at += 4;
		out[i].length = lengths[i];
		out[i].data = pack + at;
		if (lengths[i] == 0) {
			continue;
		}
		at = frpc_align(at, 8);
		out[i].data = pack + at;
		at += lengths[i];
	}
	return size;
}

/* ---- the file server ------------------------------------------------------------------------------------------ */

#define FRPC_HANDLE_REMOTECTL           0       /* the reverse handles: 0 is fixed (remotectl), the rest are ours */
#define FRPC_HANDLE_APPS_STD            1
#define FRPC_HANDLE_APPS_MEM            2
#define FRPC_HANDLE_ADSPMSGD            3
#define FRPC_HANDLES                    4
#define FRPC_FILES                      32
#define FRPC_FD_BASE                    100u    /* Android's APPS_FD_BASE: the first FILE number */
#define FRPC_DIR_BASE                   0xd15c0000ull
#define FRPC_PATH                       256
#define FRPC_OVERLAY_MAX                (4u << 20)
#define FRPC_NODES_MAX                  1024u
#define FRPC_ARCHIVE_MAGIC              "DLSNSFS1"

struct frpc_ops {
	void *(*alloc)(void *ctx, uint32_t size);       /* zeroed */
	void (*free)(void *ctx, void *p, uint32_t size);
	void (*log)(void *ctx, const char *line);
	/* apps_mem: `length` bytes of ours given to the DSP's heap (FASTRPC_RMID_INIT_MMAP); its address there, or a result */
	uint32_t (*map)(void *ctx, uint64_t length, uint32_t flags, uint64_t *dsp_address);
	void *ctx;
};

/* A file the DSP has written: it hides the archive's file of the same path (deleted: hides it as removed). */
struct frpc_node {
	struct frpc_node *next;
	uint8_t *data;
	uint32_t size, capacity;
	bool deleted, directory;
	char path[FRPC_PATH];
};

struct frpc_file {
	bool used, directory, writable, append, eof;
	const uint8_t *base;            /* reading the archive's bytes */
	uint32_t base_size;
	struct frpc_node *node;         /* or the overlay's */
	uint32_t position;
	char *names;                    /* a directory: its names, NUL-separated, taken when it was opened */
	uint32_t names_capacity, names_used, names_next, names_index;
};

struct frpc_server {
	const uint8_t *archive;
	uint32_t archive_size, count;
	struct frpc_node *overlay;
	uint32_t overlay_bytes, nodes;
	struct frpc_file files[FRPC_FILES];
	struct frpc_ops ops;
	uint32_t calls, trace;          /* trace: how many more calls to log */
};

struct frpc_call {
	struct frpc_server *s;
	const struct frpc_view *in;
	struct frpc_buf *out;
	uint32_t ins, outs, base;       /* base: 4 when the first word of the primary buffer is the method's number */
	uint32_t method;
};

static inline void
frpc_say(struct frpc_server *s, const char *line)
{
	if (s->ops.log != NULL) {
		s->ops.log(s->ops.ctx, line);
	}
}

/* -- the archive: "DLSNSFS1", u32 count, u32 size, then {u32 path_offset, path_length, data_offset, data_length}[count] */

static inline void
frpc_entry(const struct frpc_server *s, uint32_t i, const char **path, uint32_t *path_length, const uint8_t **data,
    uint32_t *size)
{
	const uint8_t *e = s->archive + 16 + 16 * i;

	*path = (const char *)s->archive + frpc_rd32(e);
	*path_length = frpc_rd32(e + 4);
	*data = s->archive + frpc_rd32(e + 8);
	*size = frpc_rd32(e + 12);
}

static inline bool
frpc_inside(uint32_t offset, uint32_t length, uint32_t size)
{
	return offset <= size && length <= size - offset;
}

static inline void frpc_server_reset(struct frpc_server *s);

static inline bool
frpc_server_init(struct frpc_server *s, const struct frpc_ops *ops, const uint8_t *archive, uint32_t size)
{
	uint32_t count;

	memset(s, 0, sizeof(*s));
	s->ops = *ops;
	if (archive == NULL || size < 16 || memcmp(archive, FRPC_ARCHIVE_MAGIC, 8) != 0 || frpc_rd32(archive + 12) != size) {
		return false;
	}
	count = frpc_rd32(archive + 8);
	if (count > (size - 16) / 16) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = archive + 16 + 16 * i;
		uint32_t path = frpc_rd32(e), path_length = frpc_rd32(e + 4);

		if (!frpc_inside(path, path_length, size) || !frpc_inside(frpc_rd32(e + 8), frpc_rd32(e + 12), size) ||
		    path_length == 0 || path_length >= FRPC_PATH || archive[path] != '/') {
			return false;
		}
	}
	s->archive = archive;
	s->archive_size = size;
	s->count = count;
	return true;
}

/* -- paths: absolute, "." and empty segments dropped, ".." refused, Android's aliases folded into one name */

static const struct {
	const char *from, *to;
} frpc_aliases[] = {
	{ "/system/vendor", "/vendor" },
	{ "/persist", "/mnt/vendor/persist" },
};

static const char *const frpc_writable[] = { "/mnt/vendor/persist", "/data/vendor/sensors" };

/* Whether `path` is `prefix` or lies under it. */
static inline bool
frpc_under(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);

	return strncmp(path, prefix, n) == 0 && (path[n] == '/' || path[n] == 0);
}

static inline uint32_t
frpc_path(const char *name, char *out)
{
	char work[FRPC_PATH];
	uint32_t o = 0;

	if (name[0] != '/') {
		return FRPC_ENOENT;
	}
	for (const char *p = name; *p != 0;) {
		const char *segment;
		uint32_t n;

		while (*p == '/') {
			p++;
		}
		segment = p;
		while (*p != 0 && *p != '/') {
			p++;
		}
		n = (uint32_t)(p - segment);
		if (n == 0 || (n == 1 && segment[0] == '.')) {
			continue;
		}
		if (n == 2 && segment[0] == '.' && segment[1] == '.') {
			return FRPC_EINVAL;
		}
		if (o + 1 + n >= FRPC_PATH) {
			return FRPC_ENAMETOOLONG;
		}
		work[o++] = '/';
		memcpy(work + o, segment, n);
		o += n;
	}
	if (o == 0) {
		work[o++] = '/';
	}
	work[o] = 0;
	for (uint32_t a = 0; a < sizeof(frpc_aliases) / sizeof(frpc_aliases[0]); a++) {
		size_t from = strlen(frpc_aliases[a].from), to = strlen(frpc_aliases[a].to);

		if (!frpc_under(work, frpc_aliases[a].from)) {
			continue;
		}
		if (o - from + to >= FRPC_PATH) {
			return FRPC_ENAMETOOLONG;
		}
		memcpy(out, frpc_aliases[a].to, to);
		memcpy(out + to, work + from, o - from + 1);
		return 0;
	}
	memcpy(out, work, o + 1);
	return 0;
}

static inline bool
frpc_is_writable(const char *path)
{
	for (uint32_t i = 0; i < sizeof(frpc_writable) / sizeof(frpc_writable[0]); i++) {
		if (frpc_under(path, frpc_writable[i]) && strlen(path) > strlen(frpc_writable[i])) {
			return true;
		}
	}
	return false;
}

/* -- lookups */

static inline struct frpc_node *
frpc_node_find(struct frpc_server *s, const char *path)
{
	for (struct frpc_node *n = s->overlay; n != NULL; n = n->next) {
		if (strcmp(n->path, path) == 0) {
			return n;
		}
	}
	return NULL;
}

static inline bool
frpc_base_find(const struct frpc_server *s, const char *path, const uint8_t **data, uint32_t *size)
{
	size_t length = strlen(path);

	for (uint32_t i = 0; i < s->count; i++) {
		const char *p;
		uint32_t pl;

		frpc_entry(s, i, &p, &pl, data, size);
		if (pl == length && memcmp(p, path, length) == 0) {
			return true;
		}
	}
	return false;
}

/* Whether some file, of the archive or the overlay, lies under `path` (the root always does). */
static inline bool
frpc_has_children(struct frpc_server *s, const char *path)
{
	size_t length = strcmp(path, "/") == 0 ? 0 : strlen(path);

	for (uint32_t i = 0; i < s->count; i++) {
		const char *p;
		const uint8_t *d;
		uint32_t pl, size;

		frpc_entry(s, i, &p, &pl, &d, &size);
		if (pl > length && memcmp(p, path, length) == 0 && p[length] == '/') {
			return true;
		}
	}
	for (struct frpc_node *n = s->overlay; n != NULL; n = n->next) {
		if (!n->deleted && strncmp(n->path, path, length) == 0 && n->path[length] == '/') {
			return true;
		}
	}
	return length == 0;
}

struct frpc_found {
	bool exists, directory;
	const uint8_t *data;
	uint32_t size;
	struct frpc_node *node;         /* the overlay's entry for the path, live or deleted */
};

static inline void
frpc_find(struct frpc_server *s, const char *path, struct frpc_found *f)
{
	memset(f, 0, sizeof(*f));
	f->node = frpc_node_find(s, path);
	if (f->node != NULL && !f->node->deleted) {
		f->exists = true;
		f->directory = f->node->directory;
		f->data = f->node->data;
		f->size = f->node->size;
		return;
	}
	if (f->node == NULL && frpc_base_find(s, path, &f->data, &f->size)) {
		f->exists = true;
		return;
	}
	f->directory = frpc_has_children(s, path);
	f->exists = f->directory;
}

/* -- the overlay */

static inline struct frpc_node *
frpc_node_make(struct frpc_server *s, const char *path)
{
	struct frpc_node *n, **tail = &s->overlay;

	if (s->nodes >= FRPC_NODES_MAX || (n = s->ops.alloc(s->ops.ctx, sizeof(*n))) == NULL) {
		return NULL;
	}
	memcpy(n->path, path, strlen(path) + 1);
	while (*tail != NULL) {
		tail = &(*tail)->next;
	}
	*tail = n;
	s->nodes++;
	return n;
}

static inline void
frpc_node_empty(struct frpc_server *s, struct frpc_node *n)
{
	if (n->data != NULL) {
		s->ops.free(s->ops.ctx, n->data, n->capacity);
		s->overlay_bytes -= n->capacity;
	}
	n->data = NULL;
	n->size = n->capacity = 0;
}

/* Room for `size` bytes, within the overlay's bound. */
static inline uint32_t
frpc_node_grow(struct frpc_server *s, struct frpc_node *n, uint32_t size)
{
	uint32_t capacity = n->capacity < 256 ? 256 : n->capacity;
	uint8_t *data;

	if (size <= n->capacity) {
		return 0;
	}
	while (capacity < size) {
		capacity = capacity > FRPC_OVERLAY_MAX ? size : capacity * 2;
	}
	if (capacity - n->capacity > FRPC_OVERLAY_MAX - s->overlay_bytes) {
		capacity = size;
	}
	if (capacity - n->capacity > FRPC_OVERLAY_MAX - s->overlay_bytes) {
		return FRPC_ENOSPC;
	}
	if ((data = s->ops.alloc(s->ops.ctx, capacity)) == NULL) {
		return FRPC_AEE_ENOMEMORY;
	}
	if (n->data != NULL) {
		memcpy(data, n->data, n->size);
		s->ops.free(s->ops.ctx, n->data, n->capacity);
	}
	s->overlay_bytes += capacity - n->capacity;
	n->data = data;
	n->capacity = capacity;
	return 0;
}

/* The overlay's live node for `path`, holding the bytes it had (`keep`) or none. */
static inline uint32_t
frpc_node_take(struct frpc_server *s, const char *path, const struct frpc_found *f, bool keep, struct frpc_node **out)
{
	struct frpc_node *n = f->node != NULL ? f->node : frpc_node_make(s, path);
	uint32_t error;

	if (n == NULL) {
		return FRPC_AEE_ENOMEMORY;
	}
	if (!keep || n->deleted) {
		n->size = 0;
	}
	if (keep && f->exists && f->node == NULL && f->size != 0) {
		if ((error = frpc_node_grow(s, n, f->size)) != 0) {
			return error;
		}
		memcpy(n->data, f->data, f->size);
		n->size = f->size;
	}
	n->deleted = false;
	*out = n;
	return 0;
}

/* -- open files */

static inline struct frpc_file *
frpc_file(struct frpc_server *s, uint32_t fd, bool directory)
{
	uint32_t i = fd - FRPC_FD_BASE;

	if (fd < FRPC_FD_BASE || i >= FRPC_FILES || !s->files[i].used || s->files[i].directory != directory) {
		return NULL;
	}
	return &s->files[i];
}

static inline struct frpc_file *
frpc_file_new(struct frpc_server *s, uint32_t *fd)
{
	for (uint32_t i = 0; i < FRPC_FILES; i++) {
		if (!s->files[i].used) {
			memset(&s->files[i], 0, sizeof(s->files[i]));
			s->files[i].used = true;
			*fd = FRPC_FD_BASE + i;
			return &s->files[i];
		}
	}
	return NULL;
}

static inline void
frpc_file_close(struct frpc_server *s, struct frpc_file *f)
{
	if (f->names != NULL) {
		s->ops.free(s->ops.ctx, f->names, f->names_capacity);
	}
	memset(f, 0, sizeof(*f));
}

static inline const uint8_t *
frpc_file_bytes(const struct frpc_file *f, uint32_t *size)
{
	*size = f->node != NULL ? f->node->size : f->base_size;
	return f->node != NULL ? f->node->data : f->base;
}

static inline void frpc_server_reset(struct frpc_server *s)
{
	for (uint32_t i = 0; i < FRPC_FILES; i++) {
		if (s->files[i].used) {
			frpc_file_close(s, &s->files[i]);
		}
	}
	while (s->overlay != NULL) {
		struct frpc_node *n = s->overlay;

		s->overlay = n->next;
		frpc_node_empty(s, n);
		s->ops.free(s->ops.ctx, n, sizeof(*n));
	}
	s->nodes = 0;
}

/* -- the calls' buffers */

static inline uint32_t
frpc_word(const struct frpc_call *c, uint32_t i)
{
	return frpc_rd32(c->in[0].data + c->base + 4 * i);
}

static inline uint64_t
frpc_word64(const struct frpc_call *c, uint32_t offset)
{
	return frpc_rd64(c->in[0].data + c->base + offset);
}

/* In-buffer i as a C string: NUL-terminated within its bytes, or NULL. */
static inline const char *
frpc_string(const struct frpc_call *c, uint32_t i)
{
	const struct frpc_view *v = &c->in[i];

	if (i >= c->ins || v->length == 0 || v->data[v->length - 1] != 0) {
		return NULL;
	}
	return (const char *)v->data;
}

static inline uint8_t *
frpc_out(const struct frpc_call *c, uint32_t i)
{
	return (uint8_t *)c->out[i].data;
}

static inline void
frpc_set32(const struct frpc_call *c, uint32_t i, uint32_t offset, uint32_t value)
{
	if (offset + 4 <= c->out[i].length) {
		frpc_wr32(frpc_out(c, i) + offset, value);
	}
}

static inline void
frpc_set64(const struct frpc_call *c, uint32_t i, uint32_t offset, uint64_t value)
{
	if (offset + 8 <= c->out[i].length) {
		frpc_wr64(frpc_out(c, i) + offset, value);
	}
}

static inline void
frpc_trace(const struct frpc_call *c, const char *what, const char *name, uint32_t result)
{
	char line[FRPC_PATH + 96];

	if (c->s->trace == 0) {
		return;
	}
	c->s->trace--;
	snprintf(line, sizeof(line), "%s %s -> %u", what, name, result);
	frpc_say(c->s, line);
}

/* -- remotectl (handle 0): open(in string name, rout long handle, rout sequence<char> dlerror, rout long nErr) */

static const char *const frpc_interface_names[FRPC_HANDLES] = { "remotectl", "apps_std", "apps_mem", "adspmsgd_apps" };

static inline uint32_t
frpc_remotectl_open(struct frpc_call *c)
{
	const char *name = frpc_string(c, 1);

	if (name == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	memset(frpc_out(c, 1), 0, c->out[1].length);
	for (uint32_t h = 1; h < FRPC_HANDLES; h++) {
		if (strcmp(name, frpc_interface_names[h]) == 0) {
			frpc_set32(c, 0, 0, h);
			frpc_set32(c, 0, 4, 0);
			frpc_trace(c, "remotectl open", name, h);
			return 0;
		}
	}
	frpc_set32(c, 0, 0, 0);
	frpc_set32(c, 0, 4, (uint32_t)-5);
	{
		char line[FRPC_PATH + 64];

		snprintf(line, sizeof(line), "remotectl: the DSP asks for an interface nobody here serves: \"%s\"", name);
		frpc_say(c->s, line);
	}
	return (uint32_t)-5;            /* hexagonrpcd's answer, and Android's mod_table for an unknown name */
}

/* close(in long handle, rout sequence<char> errStr, rout long nErr) */
static inline uint32_t
frpc_remotectl_close(struct frpc_call *c)
{
	memset(frpc_out(c, 1), 0, c->out[1].length);
	frpc_set32(c, 0, 0, 0);
	return 0;
}

/* -- apps_std (apps_std.idl): FILE is a long, DIR a u64 */

static inline uint32_t
frpc_open(struct frpc_call *c, const char *name, const char *mode, uint32_t *fd)
{
	struct frpc_server *s = c->s;
	char path[FRPC_PATH];
	struct frpc_found f;
	struct frpc_file *file;
	bool write = mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+') != NULL, create = mode[0] == 'w' || mode[0] == 'a';
	uint32_t error = frpc_path(name, path);

	if (error != 0) {
		return error;
	}
	frpc_find(s, path, &f);
	if (f.exists && f.directory) {
		return FRPC_EISDIR;
	}
	if (!f.exists && !create) {
		return FRPC_ENOENT;
	}
	if (write && !frpc_is_writable(path)) {
		return FRPC_EACCES;
	}
	if ((file = frpc_file_new(s, fd)) == NULL) {
		return FRPC_EMFILE;
	}
	file->base = f.data;
	file->base_size = f.size;
	file->node = f.exists && f.node != NULL && !f.node->deleted ? f.node : NULL;
	file->writable = write;
	file->append = mode[0] == 'a';
	if (write && (error = frpc_node_take(s, path, &f, mode[0] != 'w', &file->node)) != 0) {
		frpc_file_close(s, file);
		return error;
	}
	file->position = file->append ? file->node->size : 0;
	return 0;
}

/* fopen(in string name, in string mode, rout FILE psout) */
static inline uint32_t
frpc_std_fopen(struct frpc_call *c)
{
	const char *name = frpc_string(c, 1), *mode = frpc_string(c, 2);
	uint32_t fd = 0, result;

	if (name == NULL || mode == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	result = frpc_open(c, name, mode, &fd);
	frpc_set32(c, 0, 0, fd);
	frpc_trace(c, "fopen", name, result);
	return result;
}

/* The search path of ADSP_LIBRARY_PATH as the vendor's libadsprpc makes it (an empty first entry: absolute names). */
static const char *const frpc_library_path[] = { "", "/vendor/dsp/adsp", "/vendor/dsp", "/vendor/lib/rfsa/adsp" };

/* fopen_with_env(in string envvarname, in string delim, in string name, in string mode, rout FILE psout) */
static inline uint32_t
frpc_std_fopen_with_env(struct frpc_call *c)
{
	const char *variable = frpc_string(c, 1), *name = frpc_string(c, 3), *mode = frpc_string(c, 4);
	uint32_t fd = 0, result = FRPC_ENOENT;

	if (variable == NULL || frpc_string(c, 2) == NULL || name == NULL || mode == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	for (uint32_t i = 0; i < sizeof(frpc_library_path) / sizeof(frpc_library_path[0]) && result == FRPC_ENOENT; i++) {
		char full[FRPC_PATH];

		if (snprintf(full, sizeof(full), "%s%s%s", frpc_library_path[i], frpc_library_path[i][0] ? "/" : "", name) >=
		    (int)sizeof(full)) {
			continue;
		}
		result = frpc_open(c, full, mode, &fd);
	}
	frpc_set32(c, 0, 0, fd);
	frpc_trace(c, "fopen_with_env", name, result);
	return result;
}

/* fclose(in FILE) */
static inline uint32_t
frpc_std_fclose(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);

	if (f == NULL) {
		return FRPC_EBADF;
	}
	frpc_file_close(c->s, f);
	return 0;
}

/* fflush, fsync, clearerr (in FILE): nothing to do */
static inline uint32_t
frpc_std_nothing(struct frpc_call *c)
{
	return frpc_file(c->s, frpc_word(c, 0), false) == NULL ? FRPC_EBADF : 0;
}

/* fread(in FILE, rout sequence<byte> buf, rout long bytesRead, rout long bEOF) */
static inline uint32_t
frpc_std_fread(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);
	uint32_t wanted = c->out[1].length, size, count;
	const uint8_t *bytes;

	if (f == NULL) {
		return FRPC_EBADF;
	}
	bytes = frpc_file_bytes(f, &size);
	count = f->position >= size ? 0 : (size - f->position < wanted ? size - f->position : wanted);
	if (count != 0) {
		memcpy(frpc_out(c, 1), bytes + f->position, count);
	}
	f->position += count;
	f->eof = count < wanted;
	frpc_set32(c, 0, 0, count);
	frpc_set32(c, 0, 4, f->eof);
	return 0;
}

/* fgets(in FILE, rout sequence<byte> buf, rout long bEOF): a line, NUL-terminated */
static inline uint32_t
frpc_std_fgets(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);
	uint32_t room = c->out[1].length, size, count = 0;
	const uint8_t *bytes;

	if (f == NULL) {
		return FRPC_EBADF;
	}
	if (room == 0) {
		return FRPC_AEE_EBADPARM;
	}
	bytes = frpc_file_bytes(f, &size);
	while (count + 1 < room && f->position < size) {
		uint8_t b = bytes[f->position++];

		frpc_out(c, 1)[count++] = b;
		if (b == '\n') {
			break;
		}
	}
	frpc_out(c, 1)[count] = 0;
	f->eof = count == 0;
	frpc_set32(c, 0, 0, f->eof);
	return 0;
}

/* fwrite(in FILE, in sequence<byte> buf, rout long bytesWritten, rout long bEOF) */
static inline uint32_t
frpc_std_fwrite(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);
	uint32_t length = c->in[1].length, error;

	if (f == NULL || !f->writable || f->node == NULL) {
		return FRPC_EBADF;
	}
	f->position = f->append ? f->node->size : f->position;
	if (f->position > FRPC_OVERLAY_MAX || length > FRPC_OVERLAY_MAX - f->position) {
		return FRPC_ENOSPC;
	}
	if ((error = frpc_node_grow(c->s, f->node, f->position + length)) != 0) {
		frpc_set32(c, 0, 0, 0);
		return error;
	}
	if (f->position > f->node->size) {
		memset(f->node->data + f->node->size, 0, f->position - f->node->size);
	}
	memcpy(f->node->data + f->position, c->in[1].data, length);
	f->position += length;
	f->node->size = f->position > f->node->size ? f->position : f->node->size;
	frpc_set32(c, 0, 0, length);
	frpc_set32(c, 0, 4, 0);
	return 0;
}

/* ftell(in FILE, rout long pos) */
static inline uint32_t
frpc_std_ftell(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);

	if (f == NULL) {
		return FRPC_EBADF;
	}
	frpc_set32(c, 0, 0, f->position);
	return 0;
}

/* fseek(in FILE, in long offset, in SEEK whence): SET, CUR, END */
static inline uint32_t
frpc_std_fseek(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);
	int64_t offset = (int32_t)frpc_word(c, 1), from;
	uint32_t whence = frpc_word(c, 2), size;

	if (f == NULL) {
		return FRPC_EBADF;
	}
	frpc_file_bytes(f, &size);
	if (whence > 2) {
		return FRPC_EINVAL;
	}
	from = whence == 0 ? 0 : whence == 1 ? (int64_t)f->position : (int64_t)size;
	if (from + offset < 0 || from + offset > FRPC_OVERLAY_MAX) {
		return FRPC_EINVAL;
	}
	f->position = (uint32_t)(from + offset);
	f->eof = false;
	return 0;
}

/* flen(in FILE, rout uint64 len) */
static inline uint32_t
frpc_std_flen(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);
	uint32_t size;

	if (f == NULL) {
		return FRPC_EBADF;
	}
	frpc_file_bytes(f, &size);
	frpc_set64(c, 0, 0, size);
	return 0;
}

/* rewind(in FILE) */
static inline uint32_t
frpc_std_rewind(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);

	if (f == NULL) {
		return FRPC_EBADF;
	}
	f->position = 0;
	f->eof = false;
	return 0;
}

/* feof(in FILE, rout long bEOF) and ferror(in FILE, rout long err) */
static inline uint32_t
frpc_std_feof(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);

	if (f == NULL) {
		return FRPC_EBADF;
	}
	frpc_set32(c, 0, 0, c->method == 12 ? f->eof : 0);
	return 0;
}

/* print_string(in string): the DSP's own words, for the log */
static inline uint32_t
frpc_std_print_string(struct frpc_call *c)
{
	const char *text = frpc_string(c, 1);
	char line[320];

	if (text == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	snprintf(line, sizeof(line), "the sensors PD says: %s", text);
	frpc_say(c->s, line);
	return 0;
}

/* getenv(in string name, rout string val, rout long valLenReq): only the two search paths Android's library sets */
static const struct {
	const char *name, *value;
} frpc_environment[] = {
	{ "ADSP_LIBRARY_PATH", ";/vendor/dsp/adsp;/vendor/dsp;/vendor/lib/rfsa/adsp" },
	{ "ADSP_AVS_CFG_PATH", ";/vendor/etc/acdbdata/" },
};

static inline uint32_t
frpc_std_getenv(struct frpc_call *c)
{
	const char *name = frpc_string(c, 1);
	uint32_t room = c->out[1].length;

	if (name == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	for (uint32_t i = 0; i < sizeof(frpc_environment) / sizeof(frpc_environment[0]); i++) {
		uint32_t length = (uint32_t)strlen(frpc_environment[i].value) + 1, n = length < room ? length : room;

		if (strcmp(name, frpc_environment[i].name) != 0) {
			continue;
		}
		if (n != 0) {
			memcpy(frpc_out(c, 1), frpc_environment[i].value, n);
			frpc_out(c, 1)[n - 1] = 0;
		}
		frpc_set32(c, 0, 0, length);
		return 0;
	}
	frpc_trace(c, "getenv", name, FRPC_EUNSET);
	return FRPC_EUNSET;
}

/* setenv, unsetenv, rmdir: accepted, nothing kept */
static inline uint32_t
frpc_std_accept(struct frpc_call *c)
{
	(void)c;
	return 0;
}

/* fileExists(in string path, rout boolean exists) */
static inline uint32_t
frpc_std_file_exists(struct frpc_call *c)
{
	const char *name = frpc_string(c, 1);
	char path[FRPC_PATH];
	struct frpc_found f;

	if (name == NULL || c->out[0].length < 1) {
		return FRPC_AEE_EBADPARM;
	}
	f.exists = false;
	if (frpc_path(name, path) == 0) {
		frpc_find(c->s, path, &f);
	}
	frpc_out(c, 0)[0] = f.exists;
	frpc_trace(c, "fileExists", name, f.exists);
	return 0;
}

/* fremove(in string name) */
static inline uint32_t
frpc_std_fremove(struct frpc_call *c)
{
	const char *name = frpc_string(c, 1);
	char path[FRPC_PATH];
	struct frpc_found f;
	uint32_t error;

	if (name == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	if ((error = frpc_path(name, path)) != 0) {
		return error;
	}
	frpc_find(c->s, path, &f);
	error = !f.exists ? FRPC_ENOENT : f.directory ? FRPC_EISDIR : !frpc_is_writable(path) ? FRPC_EACCES : 0;
	if (error == 0 && f.node == NULL && (f.node = frpc_node_make(c->s, path)) == NULL) {
		error = FRPC_AEE_ENOMEMORY;
	}
	if (error == 0) {
		frpc_node_empty(c->s, f.node);
		f.node->deleted = true;
	}
	frpc_trace(c, "fremove", name, error);
	return error;
}

/* frename(in string oldname, in string newname) */
static inline uint32_t
frpc_std_frename(struct frpc_call *c)
{
	const char *from = frpc_string(c, 1), *to = frpc_string(c, 2);
	char old_path[FRPC_PATH], new_path[FRPC_PATH];
	struct frpc_found f, t;
	struct frpc_node *n;
	uint32_t error;

	if (from == NULL || to == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	if ((error = frpc_path(from, old_path)) != 0 || (error = frpc_path(to, new_path)) != 0) {
		return error;
	}
	if (strcmp(old_path, new_path) == 0) {
		return 0;
	}
	frpc_find(c->s, old_path, &f);
	frpc_find(c->s, new_path, &t);
	error = !f.exists ? FRPC_ENOENT : f.directory || t.directory ? FRPC_EISDIR :
	    !frpc_is_writable(old_path) || !frpc_is_writable(new_path) ? FRPC_EACCES : 0;
	if (error == 0 && (error = frpc_node_take(c->s, new_path, &t, false, &n)) == 0) {
		struct frpc_node *old = f.node != NULL ? f.node : frpc_node_make(c->s, old_path);

		if (old == NULL || (error = frpc_node_grow(c->s, n, f.size)) != 0) {
			error = error != 0 ? error : FRPC_AEE_ENOMEMORY;
		} else {
			if (f.size != 0) {
				memcpy(n->data, f.data, f.size);
			}
			n->size = f.size;
			frpc_node_empty(c->s, old);
			old->deleted = true;
		}
	}
	frpc_trace(c, "frename", to, error);
	return error;
}

/* mkdir(in string name, in long mode) */
static inline uint32_t
frpc_std_mkdir(struct frpc_call *c)
{
	const char *name = frpc_string(c, 1);
	char path[FRPC_PATH];
	struct frpc_found f;
	uint32_t error;

	if (name == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	if ((error = frpc_path(name, path)) != 0) {
		return error;
	}
	if (!frpc_is_writable(path)) {
		return FRPC_EACCES;
	}
	frpc_find(c->s, path, &f);
	if (f.exists) {
		return f.directory ? 0 : FRPC_EINVAL;
	}
	if (f.node == NULL && (f.node = frpc_node_make(c->s, path)) == NULL) {
		return FRPC_AEE_ENOMEMORY;
	}
	f.node->deleted = false;
	f.node->directory = true;
	return 0;
}

/* -- directories: the names are taken at opendir (".", "..", then the archive's, then the overlay's, each once) */

static inline bool
frpc_names_has(const struct frpc_file *d, const char *name, uint32_t length)
{
	for (uint32_t at = 0; at < d->names_used;) {
		size_t n = strlen(d->names + at);

		if (n == length && memcmp(d->names + at, name, length) == 0) {
			return true;
		}
		at += (uint32_t)n + 1;
	}
	return false;
}

static inline void
frpc_names_add(struct frpc_file *d, const char *name, uint32_t length)
{
	if (length == 0 || d->names_used + length + 1 > d->names_capacity || frpc_names_has(d, name, length)) {
		return;
	}
	memcpy(d->names + d->names_used, name, length);
	d->names[d->names_used + length] = 0;
	d->names_used += length + 1;
}

/* The name of `path`'s child that `full` lies in, or 0 if it lies elsewhere. */
static inline uint32_t
frpc_child(const char *path, size_t length, const char *full, uint32_t full_length, const char **child)
{
	uint32_t n = 0;

	if (full_length <= length || memcmp(full, path, length) != 0 || full[length] != '/') {
		return 0;
	}
	*child = full + length + 1;
	while (length + 1 + n < full_length && (*child)[n] != '/') {
		n++;
	}
	return n;
}

static inline void
frpc_names_take(struct frpc_server *s, struct frpc_file *d, const char *path, bool count_only, uint32_t *bytes)
{
	size_t length = strcmp(path, "/") == 0 ? 0 : strlen(path);
	const char *child;
	uint32_t n;

	*bytes = 6;                     /* ".", ".." */
	if (!count_only) {
		frpc_names_add(d, ".", 1);
		frpc_names_add(d, "..", 2);
	}
	for (uint32_t i = 0; i < s->count; i++) {
		const char *p;
		const uint8_t *data;
		uint32_t pl, size;
		struct frpc_node *hidden;
		char full[FRPC_PATH];

		frpc_entry(s, i, &p, &pl, &data, &size);
		if ((n = frpc_child(path, length, p, pl, &child)) == 0) {
			continue;
		}
		*bytes += n + 1;
		memcpy(full, p, pl);
		full[pl] = 0;
		hidden = length + 1 + n == pl ? frpc_node_find(s, full) : NULL;
		if (!count_only && (hidden == NULL || !hidden->deleted)) {
			frpc_names_add(d, child, n);
		}
	}
	for (struct frpc_node *node = s->overlay; node != NULL; node = node->next) {
		if (node->deleted || (n = frpc_child(path, length, node->path, (uint32_t)strlen(node->path), &child)) == 0) {
			continue;
		}
		*bytes += n + 1;
		if (!count_only) {
			frpc_names_add(d, child, n);
		}
	}
}

/* opendir(in string name, rout DIR dir) */
static inline uint32_t
frpc_std_opendir(struct frpc_call *c)
{
	struct frpc_server *s = c->s;
	const char *name = frpc_string(c, 1);
	char path[FRPC_PATH];
	struct frpc_found f;
	struct frpc_file *d;
	uint32_t error, fd, bytes;

	if (name == NULL) {
		return FRPC_AEE_EBADPARM;
	}
	if ((error = frpc_path(name, path)) != 0) {
		return error;
	}
	frpc_find(s, path, &f);
	error = !f.exists ? FRPC_ENOENT : !f.directory ? FRPC_ENOTDIR : 0;
	if (error == 0 && (d = frpc_file_new(s, &fd)) == NULL) {
		error = FRPC_EMFILE;
	}
	if (error != 0) {
		frpc_trace(c, "opendir", name, error);
		return error;
	}
	d->directory = true;
	frpc_names_take(s, d, path, true, &bytes);
	if ((d->names = s->ops.alloc(s->ops.ctx, bytes)) == NULL) {
		frpc_file_close(s, d);
		return FRPC_AEE_ENOMEMORY;
	}
	d->names_capacity = bytes;
	frpc_names_take(s, d, path, false, &bytes);
	frpc_set64(c, 0, 0, FRPC_DIR_BASE + fd);
	frpc_trace(c, "opendir", name, 0);
	return 0;
}

static inline struct frpc_file *
frpc_dir(struct frpc_call *c)
{
	uint64_t handle = frpc_word64(c, 0);

	if (handle < FRPC_DIR_BASE + FRPC_FD_BASE || handle >= FRPC_DIR_BASE + FRPC_FD_BASE + FRPC_FILES) {
		return NULL;
	}
	return frpc_file(c->s, (uint32_t)(handle - FRPC_DIR_BASE), true);
}

/* closedir(in DIR dir) */
static inline uint32_t
frpc_std_closedir(struct frpc_call *c)
{
	struct frpc_file *d = frpc_dir(c);

	if (d == NULL) {
		return FRPC_EBADF;
	}
	frpc_file_close(c->s, d);
	return 0;
}

/* readdir(in DIR dir, rout DIRENT {long ino; char name[255]}, rout long bEOF): the struct padded to 260, bEOF at 260 */
static inline uint32_t
frpc_std_readdir(struct frpc_call *c)
{
	struct frpc_file *d = frpc_dir(c);
	uint8_t *out = frpc_out(c, 0);

	if (d == NULL) {
		return FRPC_EBADF;
	}
	if (c->out[0].length < 264) {
		return FRPC_AEE_EBADPARM;
	}
	memset(out, 0, 264);
	if (d->names_next >= d->names_used) {
		frpc_wr32(out + 260, 1);
		return 0;
	}
	{
		const char *name = d->names + d->names_next;
		size_t n = strlen(name);

		d->names_next += (uint32_t)n + 1;
		d->names_index++;
		frpc_wr32(out, d->names_index);
		memcpy(out + 4, name, n < 254 ? n : 254);
	}
	return 0;
}

/* stat(in string name, rout STAT): tsz, dev, ino (u64), mode, nlink (u32), rdev, size, then six times (u64): 96 bytes */
static inline uint32_t
frpc_std_stat(struct frpc_call *c)
{
	const char *name = frpc_string(c, 1);
	char path[FRPC_PATH];
	struct frpc_found f;
	uint32_t error;

	if (name == NULL || c->out[0].length < 96) {
		return FRPC_AEE_EBADPARM;
	}
	if ((error = frpc_path(name, path)) != 0) {
		return error;
	}
	frpc_find(c->s, path, &f);
	frpc_trace(c, "stat", name, f.exists ? 0 : FRPC_ENOENT);
	if (!f.exists) {
		return FRPC_ENOENT;
	}
	memset(frpc_out(c, 0), 0, 96);
	frpc_set32(c, 0, 24, f.directory ? 0040755u : 0100644u);
	frpc_set32(c, 0, 28, 1);
	frpc_set64(c, 0, 40, f.directory ? 0 : f.size);
	return 0;
}

/* ftrunc(in FILE, in long offset) */
static inline uint32_t
frpc_std_ftrunc(struct frpc_call *c)
{
	struct frpc_file *f = frpc_file(c->s, frpc_word(c, 0), false);
	uint32_t length = frpc_word(c, 1), error;

	if (f == NULL || !f->writable || f->node == NULL) {
		return FRPC_EBADF;
	}
	if ((error = frpc_node_grow(c->s, f->node, length)) != 0) {
		return error;
	}
	if (length > f->node->size) {
		memset(f->node->data + f->node->size, 0, length - f->node->size);
	}
	f->node->size = length;
	return 0;
}

/* -- apps_mem (apps_mem.idl) */

#define FRPC_MMAP_ADD_PAGES             0x1000u /* ADSP_MMAP_ADD_PAGES: pages of ours for the DSP's heap */

/* request_map64(in long heapid, in uint32 ion_flags, in uint32 rflags, in uint64 vin, in int64 len,
 *               rout uint64 vapps, rout uint64 vadsp): the 64-bit ones on 8 bytes, so the primary buffer is 32 */
static inline uint32_t
frpc_mem_map64(struct frpc_call *c)
{
	uint32_t flags = frpc_word(c, 2), result;
	uint64_t length = frpc_word64(c, 24), address = 0;

	if (!(flags & FRPC_MMAP_ADD_PAGES) || c->s->ops.map == NULL) {
		frpc_trace(c, "apps_mem map64", "(not pages for the heap)", FRPC_AEE_EUNSUPPORTED);
		return FRPC_AEE_EUNSUPPORTED;
	}
	result = c->s->ops.map(c->s->ops.ctx, length, flags, &address);
	frpc_set64(c, 0, 0, 0);
	frpc_set64(c, 0, 8, address);
	return result;
}

/* request_map(in long heapid, in uint32 ion_flags, in uint32 rflags, in uint32 vin, in int32 len,
 *             rout uint32 vapps, rout uint32 vadsp) */
static inline uint32_t
frpc_mem_map(struct frpc_call *c)
{
	uint32_t flags = frpc_word(c, 2), result;
	uint64_t address = 0;

	if (!(flags & FRPC_MMAP_ADD_PAGES) || c->s->ops.map == NULL) {
		return FRPC_AEE_EUNSUPPORTED;
	}
	result = c->s->ops.map(c->s->ops.ctx, frpc_word(c, 4), flags, &address);
	frpc_set32(c, 0, 0, 0);
	frpc_set32(c, 0, 4, (uint32_t)address);
	return result;
}

/* -- adspmsgd_apps: log(in sequence<octet>): the DSP's log messages, their printable runs */
static inline uint32_t
frpc_msgd_log(struct frpc_call *c)
{
	const uint8_t *b = c->in[1].data;
	char line[200];
	uint32_t n = 0;

	for (uint32_t i = 0; i <= c->in[1].length; i++) {
		bool printable = i < c->in[1].length && b[i] >= 0x20 && b[i] < 0x7f;

		if (printable && n + 1 < sizeof(line)) {
			line[n++] = (char)b[i];
			continue;
		}
		if (n >= 8) {
			line[n] = 0;
			frpc_say(c->s, line);
		}
		n = 0;
	}
	return 0;
}

/* -- the method tables: per method, its buffers (the primary ones included) and the primary buffers' least sizes */

struct frpc_method {
	uint32_t (*call)(struct frpc_call *c);
	uint8_t ins, outs;
	uint16_t prim_in, prim_out;
};

#define FRPC_NONE       { NULL, 0, 0, 0, 0 }

static const struct frpc_method frpc_remotectl_methods[] = {
	{ frpc_remotectl_open, 2, 2, 8, 8 },
	{ frpc_remotectl_close, 1, 2, 8, 4 },
};

static const struct frpc_method frpc_std_methods[] = {
	/*  0 fopen           */ { frpc_std_fopen, 3, 1, 8, 4 },
	/*  1 freopen         */ FRPC_NONE,
	/*  2 fflush          */ { frpc_std_nothing, 1, 0, 4, 0 },
	/*  3 fclose          */ { frpc_std_fclose, 1, 0, 4, 0 },
	/*  4 fread           */ { frpc_std_fread, 1, 2, 8, 8 },
	/*  5 fwrite          */ { frpc_std_fwrite, 2, 1, 8, 8 },
	/*  6 fgetpos         */ FRPC_NONE,
	/*  7 fsetpos         */ FRPC_NONE,
	/*  8 ftell           */ { frpc_std_ftell, 1, 1, 4, 4 },
	/*  9 fseek           */ { frpc_std_fseek, 1, 0, 12, 0 },
	/* 10 flen            */ { frpc_std_flen, 1, 1, 4, 8 },
	/* 11 rewind          */ { frpc_std_rewind, 1, 0, 4, 0 },
	/* 12 feof            */ { frpc_std_feof, 1, 1, 4, 4 },
	/* 13 ferror          */ { frpc_std_feof, 1, 1, 4, 4 },
	/* 14 clearerr        */ { frpc_std_nothing, 1, 0, 4, 0 },
	/* 15 print_string    */ { frpc_std_print_string, 2, 0, 4, 0 },
	/* 16 getenv          */ { frpc_std_getenv, 2, 2, 8, 4 },
	/* 17 setenv          */ { frpc_std_accept, 3, 0, 12, 0 },
	/* 18 unsetenv        */ { frpc_std_accept, 2, 0, 4, 0 },
	/* 19 fopen_with_env  */ { frpc_std_fopen_with_env, 5, 1, 16, 4 },
	/* 20 fgets           */ { frpc_std_fgets, 1, 2, 8, 4 },
	/* 21 get_search_paths_with_env */ FRPC_NONE,
	/* 22 fileExists      */ { frpc_std_file_exists, 2, 1, 4, 1 },
	/* 23 fsync           */ { frpc_std_nothing, 1, 0, 4, 0 },
	/* 24 fremove         */ { frpc_std_fremove, 2, 0, 4, 0 },
	/* 25 fdopen_decrypt  */ FRPC_NONE,
	/* 26 opendir         */ { frpc_std_opendir, 2, 1, 4, 8 },
	/* 27 closedir        */ { frpc_std_closedir, 1, 0, 8, 0 },
	/* 28 readdir         */ { frpc_std_readdir, 1, 1, 8, 264 },
	/* 29 mkdir           */ { frpc_std_mkdir, 2, 0, 8, 0 },
	/* 30 rmdir           */ { frpc_std_accept, 2, 0, 4, 0 },
	/* 31 stat            */ { frpc_std_stat, 2, 1, 4, 96 },
	/* 32 ftrunc          */ { frpc_std_ftrunc, 1, 0, 8, 0 },
	/* 33 frename         */ { frpc_std_frename, 3, 0, 8, 0 },
};

static const struct frpc_method frpc_mem_methods[] = {
	/* 0 request_map      */ { frpc_mem_map, 1, 1, 20, 8 },
	/* 1 request_unmap    */ { frpc_std_accept, 1, 0, 8, 0 },
	/* 2 request_map64    */ { frpc_mem_map64, 1, 1, 32, 16 },
	/* 3 request_unmap64  */ { frpc_std_accept, 1, 0, 16, 0 },
};

static const struct frpc_method frpc_msgd_methods[] = {
	/* 0 log              */ { frpc_msgd_log, 2, 0, 4, 0 },
};

static const struct {
	const struct frpc_method *methods;
	uint32_t count;
} frpc_interfaces[FRPC_HANDLES] = {
	{ frpc_remotectl_methods, sizeof(frpc_remotectl_methods) / sizeof(frpc_remotectl_methods[0]) },
	{ frpc_std_methods, sizeof(frpc_std_methods) / sizeof(frpc_std_methods[0]) },
	{ frpc_mem_methods, sizeof(frpc_mem_methods) / sizeof(frpc_mem_methods[0]) },
	{ frpc_msgd_methods, sizeof(frpc_msgd_methods) / sizeof(frpc_msgd_methods[0]) },
};

/*
 * One call the DSP makes of us: `in` holds its in-buffers (the primary one first, if any), `out` the out-buffers it
 * wants (primary first), zeroed, of the lengths it gave. The result goes back to it as next2's prevResult.
 */
static inline uint32_t
frpc_server_call(struct frpc_server *s, uint32_t handle, uint32_t sc, const struct frpc_view *in, struct frpc_buf *out)
{
	struct frpc_call c = { s, in, out, FRPC_SC_INBUFS(sc), FRPC_SC_OUTBUFS(sc), 0, FRPC_SC_METHOD(sc) };
	const struct frpc_method *m;

	s->calls++;
	if (FRPC_SC_HANDLES(sc) != 0) {
		return FRPC_AEE_EBADPARM;
	}
	if (handle >= FRPC_HANDLES) {
		return FRPC_AEE_EUNSUPPORTED;
	}
	if (c.method == FRPC_METHOD_EXTENDED) {
		if (c.ins == 0 || in[0].length < 4) {
			return FRPC_AEE_EBADPARM;
		}
		c.method = frpc_rd32(in[0].data);
		c.base = 4;
	}
	if (c.method >= frpc_interfaces[handle].count || frpc_interfaces[handle].methods[c.method].call == NULL) {
		return FRPC_AEE_EUNSUPPORTED;
	}
	m = &frpc_interfaces[handle].methods[c.method];
	if (c.ins != m->ins || c.outs != m->outs || (m->ins != 0 && in[0].length < c.base + m->prim_in) ||
	    (m->outs != 0 && out[0].length < m->prim_out)) {
		return FRPC_AEE_EBADPARM;
	}
	return m->call(&c);
}

#endif /* DLISA_FASTRPC_PROTO_H */
