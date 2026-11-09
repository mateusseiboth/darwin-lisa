/* SPDX-License-Identifier: MIT */
/* Host test for pac_emulate.c: LDRAA/LDRAB addressing + FJCVTZS against the host FPU. */
#include "pac_emulate.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

/* ---- memory callback ---- */
struct mem {
	uint64_t base;
	const uint64_t *words;	/* 8-byte words starting at base */
	size_t count;
	int fault_all;
};

static int
mem_load64(void *ctx, uint64_t addr, uint64_t *out)
{
	struct mem *m = ctx;
	if (m->fault_all || (addr & 7) || addr < m->base) {
		return -1;
	}
	size_t i = (size_t)((addr - m->base) / 8);
	if (i >= m->count) {
		return -1;
	}
	*out = m->words[i];
	return 0;
}

/* ---- encoders ---- */
static uint32_t
enc_ldra(unsigned key_b, int32_t offset, unsigned writeback, unsigned rn, unsigned rt)
{
	int32_t raw = (offset / 8) & 0x3FF;
	uint32_t s = (uint32_t)(raw >> 9) & 1;
	uint32_t imm9 = (uint32_t)raw & 0x1FF;
	return 0xF8200400u | key_b << 23 | s << 22 | imm9 << 12 | writeback << 11 | rn << 5 | rt;
}

static uint32_t
enc_fjcvtzs(unsigned rn, unsigned rd)
{
	return 0x1E7E0000u | rn << 5 | rd;
}

/* ---- FJCVTZS reference (host doubles) ---- */
static uint32_t
ref_toint32(double x)
{
	if (!isfinite(x)) {
		return 0;
	}
	double t = trunc(x);
	double m = fmod(t, 4294967296.0);
	if (m < 0) {
		m += 4294967296.0;
	}
	return (uint32_t)m;
}

static int
ref_exact(double x)
{
	if (!isfinite(x)) {
		return 0;
	}
	if (x == 0.0 && signbit(x)) {
		return 0;
	}
	if (trunc(x) != x) {
		return 0;
	}
	return x >= -2147483648.0 && x <= 2147483647.0;
}

static uint64_t
dbits(double x)
{
	uint64_t b;
	memcpy(&b, &x, 8);
	return b;
}

static void
test_fjcvtzs(void)
{
	static const double cases[] = {
		0.0, -0.0, 1.0, -1.0, 42.0, -42.0, 2.5, -2.5, 3.9, -3.9,
		2147483647.0, -2147483648.0, 2147483648.0, -2147483649.0,
		4294967296.0, 4294967297.5, 1e9, -1e9, 1e300, -1e300,
		0.25, -0.25, 123456.789, 4503599627370496.0 /* 2^52 */,
		1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 9007199254740992.0 /* 2^53 */,
		-9007199254740992.0, 65535.5, -65535.5, 2147483646.999,
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		double x = cases[i];
		struct pac_emulate_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.load64 = mem_load64;
		ctx.pc = 0x4000;
		ctx.nzcv = 0xF0000000;		/* all flags set, to prove they get cleared */
		ctx.d[7] = dbits(x);
		enum pac_emulate_result r = pac_emulate(enc_fjcvtzs(7, 3), &ctx);
		CHECK(r == PAC_EMULATE_OK, "fjcvtzs[%zu] not handled", i);
		uint32_t got = (uint32_t)ctx.x[3];
		uint32_t want = ref_toint32(x);
		CHECK(ctx.x[3] >> 32 == 0, "fjcvtzs[%zu] Xd upper bits not zero-extended", i);
		CHECK(got == want, "fjcvtzs[%zu] x=%g got=0x%08x want=0x%08x", i, x, got, want);
		int want_z = ref_exact(x);
		int got_z = (ctx.nzcv & (1u << 30)) != 0;
		CHECK(got_z == want_z, "fjcvtzs[%zu] x=%g Z got=%d want=%d", i, x, got_z, want_z);
		CHECK((ctx.nzcv & ~(1u << 30)) == 0, "fjcvtzs[%zu] N/C/V not cleared (nzcv=0x%08x)", i, ctx.nzcv);
		CHECK(ctx.pc == 0x4004, "fjcvtzs[%zu] pc not advanced", i);
	}
}

static void
test_ldra(void)
{
	uint64_t words[8] = { 0x1111111111111111ULL, 0x2222222222222222ULL, 0x3333333333333333ULL,
			      0x4444444444444444ULL, 0x5555555555555555ULL, 0x6666666666666666ULL,
			      0x7777777777777777ULL, 0x8888888888888888ULL };
	struct mem m = { .base = 0x80000, .words = words, .count = 8, .fault_all = 0 };

	/* W=0, positive offset: x1 = [x2 + 16] */
	{
		struct pac_emulate_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.load64 = mem_load64;
		ctx.mem_ctx = &m;
		ctx.pc = 0x4000;
		ctx.x[2] = 0x80000;
		enum pac_emulate_result r = pac_emulate(enc_ldra(0, 16, 0, 2, 1), &ctx);
		CHECK(r == PAC_EMULATE_OK, "ldra+16 not handled");
		CHECK(ctx.x[1] == words[2], "ldra+16 wrong value 0x%llx", (unsigned long long)ctx.x[1]);
		CHECK(ctx.x[2] == 0x80000, "ldra+16 base changed without writeback");
		CHECK(ctx.pc == 0x4004, "ldra+16 pc not advanced");
	}
	/* W=0, large negative offset (the real trap case): x5 = [x6 - 2048] */
	{
		struct pac_emulate_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.load64 = mem_load64;
		ctx.mem_ctx = &m;
		ctx.pc = 0x4000;
		ctx.x[6] = 0x80000 + 2048 + 24;		/* so addr = base + 24 -> words[3] */
		enum pac_emulate_result r = pac_emulate(enc_ldra(1, -2048, 0, 6, 5), &ctx);
		CHECK(r == PAC_EMULATE_OK, "ldra-2048 not handled");
		CHECK(ctx.x[5] == words[3], "ldra-2048 wrong value 0x%llx", (unsigned long long)ctx.x[5]);
	}
	/* W=1, writeback: x3 = [x4 + 8]!, x4 becomes base+8 */
	{
		struct pac_emulate_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.load64 = mem_load64;
		ctx.mem_ctx = &m;
		ctx.pc = 0x4000;
		ctx.x[4] = 0x80000;
		enum pac_emulate_result r = pac_emulate(enc_ldra(0, 8, 1, 4, 3), &ctx);
		CHECK(r == PAC_EMULATE_OK, "ldra wb not handled");
		CHECK(ctx.x[3] == words[1], "ldra wb wrong value");
		CHECK(ctx.x[4] == 0x80008, "ldra wb base not written back (0x%llx)", (unsigned long long)ctx.x[4]);
	}
	/* Rn==31 means SP as the base */
	{
		struct pac_emulate_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.load64 = mem_load64;
		ctx.mem_ctx = &m;
		ctx.pc = 0x4000;
		ctx.sp = 0x80000 + 32;
		enum pac_emulate_result r = pac_emulate(enc_ldra(0, 0, 0, 31, 9), &ctx);
		CHECK(r == PAC_EMULATE_OK, "ldra sp not handled");
		CHECK(ctx.x[9] == words[4], "ldra sp wrong value");
	}
	/* Rt==31 discards the loaded value (XZR) */
	{
		struct pac_emulate_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.load64 = mem_load64;
		ctx.mem_ctx = &m;
		ctx.pc = 0x4000;
		ctx.x[2] = 0x80000;
		enum pac_emulate_result r = pac_emulate(enc_ldra(0, 0, 0, 2, 31), &ctx);
		CHECK(r == PAC_EMULATE_OK, "ldra xzr not handled");
		CHECK(ctx.pc == 0x4004, "ldra xzr pc not advanced");
	}
	/* fault propagates */
	{
		struct mem fm = { .base = 0x80000, .words = words, .count = 8, .fault_all = 1 };
		struct pac_emulate_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.load64 = mem_load64;
		ctx.mem_ctx = &fm;
		ctx.pc = 0x4000;
		ctx.x[2] = 0x80000;
		enum pac_emulate_result r = pac_emulate(enc_ldra(0, 0, 0, 2, 1), &ctx);
		CHECK(r == PAC_EMULATE_FAULT, "ldra fault not reported");
		CHECK(ctx.pc == 0x4000, "ldra fault advanced pc");
	}
}

static void
test_unhandled(void)
{
	struct pac_emulate_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.load64 = mem_load64;
	CHECK(pac_emulate(0xD503201F /* NOP */, &ctx) == PAC_EMULATE_UNHANDLED, "NOP claimed handled");
	CHECK(pac_emulate(0xD65F03C0 /* RET */, &ctx) == PAC_EMULATE_UNHANDLED, "RET claimed handled");
}

int
main(void)
{
	test_ldra();
	test_fjcvtzs();
	test_unhandled();
	if (failures == 0) {
		printf("pac_emulate: all tests passed\n");
		return 0;
	}
	printf("pac_emulate: %d failure(s)\n", failures);
	return 1;
}
