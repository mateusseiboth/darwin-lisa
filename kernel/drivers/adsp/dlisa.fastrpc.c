/*
 * darwin-lisa: the file server the sensors PD on lisa's audio DSP needs, over FastRPC, in the kernel (boot-arg
 * sensors_rpc=1; inert without it). What Android's /vendor/bin/sscrpcd does (libadsp_default_listener.so +
 * libadsprpc.so over the vendor kernel's adsprpc driver), and what hexagonrpcd does on Linux:
 *
 *   1. the DSP's GLINK channel "fastrpcglink-apps-dsp" (answered by hakios.adsp.c when this is wanted);
 *   2. a context of the application processors' SMMU for the DSP's FastRPC stream 0x1800|sid (sid 5 by default: the
 *      vendor tree's shared context bank, which libadsprpc asks for the sensors PD; sensors_rpc_sid=3|4|5), with our
 *      page tables: a payload area of 1 MB at IOVA 0x80000000 (the vendor tree's qcom,iommu-dma-addr-pool) and up to
 *      31 MB behind it for apps_mem (pages for the DSP's heap). The way of hakios.wifi.c's radio_space_*(), which the
 *      phone granted for the Wi-Fi's stream;
 *   3. the calls: attach to the sensors PD (handle 1, FASTRPC_RMID_INIT_ATTACH, PD 2, as INIT_ATTACH_SNS does), open
 *      "adsp_default_listener" through remotectl and register it, init2 the listener (handle 3), then answer the PD's
 *      reverse calls through next2 for as long as the DSP runs: remotectl (open/close of our interfaces), apps_std
 *      (files and directories), apps_mem (heap pages), adspmsgd_apps (its log). The files are dlisa.fastrpc_proto.h's
 *      server over the archive fw-sensors-stage.py made (linked in: dlisa.sensors_fs.h, sensors-wire.py), under an
 *      overlay in RAM for what the DSP writes: persist is never written.
 *
 * The wire formats and the server are in dlisa.fastrpc_proto.h (host-tested: test/run-fastrpc-tests.sh); this file is
 * the kernel's side only: memory, the SMMU, the channel, the thread. kernel/drivers/adsp/README.md says where every
 * number comes from, and what the device test looks for.
 *
 * Boot-args: sensors_rpc=1; sensors_rpc_sid=N (3, 4 or 5, default 5); sensors_rpc_wait=S (for the channel, default
 * 60); sensors_rpc_tries=N (attach attempts 2 s apart, default 30); sensors_rpc_trace=N (the first N file calls in the
 * log, default 400).
 * Sysctl kern.hakios.sensors_rpc: how far it got.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>

#include "dlisa.fastrpc.h"
#include "dlisa.fastrpc_proto.h"
#if __has_include("dlisa.sensors_fs.h")
#include "dlisa.sensors_fs.h"   /* sensors-wire.py (fw-sensors-stage.py header): the archive, linked in */
#endif

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern lck_mtx_t hakios_smmu_lock;      /* hakios.platform.c: the Wi-Fi's start takes match registers and contexts too */

static LCK_GRP_DECLARE(fastrpc_lock_group, "dlisa-fastrpc");

#define PD_SENSORS              2       /* the ctx's low bits route the message to the sensors PD (adsprpc.c: fl->pd = 2) */
#define HANDLE_REMOTECTL        0
#define HANDLE_INIT             1       /* FASTRPC_INIT_HANDLE: the kernel's own calls, with pid 0 */
#define HANDLE_LISTENER         3       /* adsp_listener.idl: _const_adsp_listener_handle */
#define RMID_INIT_ATTACH        0
#define RMID_INIT_MMAP          4
#define LISTENER_INIT2          3
#define LISTENER_NEXT2          4
#define LISTENER_GET_IN_BUFS2   5
#define OUR_TGID                0x1d15  /* the "process" the PD serves: any number, the same everywhere */
#define OUR_TID                 0x1d16

#define IOVA_BASE               0x80000000ull   /* the vendor tree's qcom,iommu-dma-addr-pool for the ADSP's context banks */
#define PAYLOAD_BYTES           (1u << 20)      /* the invokes' payloads: one call at a time */
#define WINDOW_BYTES            (32u << 20)     /* the payload, then apps_mem's pages */
#define TABLES                  (2 + WINDOW_BYTES / (2u << 20)) /* the root, the second level, and a third per 2 MB */
#define PACK_BYTES              (256u << 10)    /* the reverse calls' buffers, each way */
#define NEXT2_BYTES             (64u << 10)     /* what next2 takes at once; more comes through get_in_bufs2 */

static uint32_t trace_budget = 400, sid = 5;
static char rpc_state[256] = "off (sensors_rpc=1)";

SYSCTL_DECL(_kern_hakios);
SYSCTL_STRING(_kern_hakios, OID_AUTO, sensors_rpc, CTLFLAG_RD | CTLFLAG_LOCKED, rpc_state, sizeof(rpc_state),
    "the sensors PD's FastRPC file server: how far it got");

static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[400];
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
state(const char *format, ...) __printflike(1, 2);

static void
state(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	vsnprintf(rpc_state, sizeof(rpc_state), format, list);
	va_end(list);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* The DSP reads our memory uncached (the SMMU's attribute 0): what we wrote is cleaned out first, and what it wrote
 * is read from memory, not from stale lines. */
static void
caches(vm_offset_t start, uint32_t size)
{
	for (vm_offset_t line = start & ~63ul; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

/* ---- the SMMU: a context of ours for the DSP's FastRPC stream (hakios.wifi.c radio_space_*) ----------------- */

#define SMMU_PHYS               0x15000000u
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define PTE_PAGE                (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)   /* hakios.wifi.c's RADIO_PTE */

static struct {
	vm_offset_t global, second, context;    /* the SMMU's first page, its second, our context's page */
	uint32_t page, pages, match, context_number, stream;
	uint32_t old_smr, old_s2cr, old_sctlr;
	bool found, on;
	vm_offset_t tables;                     /* TABLES of 4K: the root, the second level, the third ones */
} smmu;

static uint32_t
smmu_rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static void
smmu_wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" : : : "memory");
}

static uint64_t *
table(uint32_t n)
{
	return (uint64_t *)(smmu.tables + 4096 * n);
}

/* Which match register and context are free (with hakios_smmu_lock held). Nothing is written. */
static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts;

	if (smmu.global == 0) {
		smmu.global = ml_io_map(SMMU_PHYS, 0x2000);
	}
	scr0 = smmu_rd(smmu.global + SMMU_SCR0);
	idr0 = smmu_rd(smmu.global + SMMU_IDR0);
	idr1 = smmu_rd(smmu.global + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	if (scr0 & 1u << 3) {
		tell("fastrpc: the SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	smmu.page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu.pages = 2u << (idr1 >> 28 & 7);
	if (2 * smmu.pages * smmu.page != 0x100000) {
		tell("fastrpc: the SMMU says 2 x %u pages of %#x bytes, not the device tree's megabyte\n", smmu.pages, smmu.page);
		return false;
	}
	smmu.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = smmu_rd(smmu.global + SMMU_SMR(n)), s2cr = smmu_rd(smmu.global + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((smr & 0x7fff) ^ smmu.stream) & ~(smr >> 16 & 0x7fff)) {
				continue;
			}
			tell("fastrpc: match register %u covers stream %#x already (smr %#x, s2cr %#x): it is somebody's, and is left "
			    "alone\n", n, smmu.stream, smr, s2cr);
			return false;
		}
		smmu.match = smmu.match == ~0u ? n : smmu.match;
	}
	for (smmu.context_number = 0; smmu.context_number < contexts && used[smmu.context_number]; smmu.context_number++) {
	}
	if (smmu.match == ~0u || smmu.context_number >= contexts) {
		tell("fastrpc: the SMMU has no match register or no context left\n");
		return false;
	}
	smmu.old_smr = smmu_rd(smmu.global + SMMU_SMR(smmu.match));
	smmu.old_s2cr = smmu_rd(smmu.global + SMMU_S2CR(smmu.match));
	smmu.second = ml_io_map(SMMU_PHYS + smmu.page, 0x1000);
	smmu.context = ml_io_map(SMMU_PHYS + (smmu.pages + smmu.context_number) * smmu.page, 0x1000);
	smmu.old_sctlr = smmu_rd(smmu.context + SMMU_CB_SCTLR);
	if (smmu.old_sctlr & 1) {
		tell("fastrpc: CONTEXT %u IS SWITCHED ON although no stream goes through it: it is somebody's\n", smmu.context_number);
		return false;
	}
	smmu.found = true;
	return true;
}

/* `size` bytes of ours at `memory` behind IOVA `iova` (the third-level tables are all there from the start). */
static void
smmu_enter(uint64_t iova, vm_offset_t memory, uint32_t size)
{
	for (uint32_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = iova + offset - IOVA_BASE;

		table(2 + (uint32_t)(at >> 21))[at >> 12 & 0x1ff] = kvtophys(memory + offset) | PTE_PAGE;
	}
	caches(smmu.tables, TABLES * 4096);
}

static void
smmu_tlb(void)
{
	smmu_wr(smmu.context + SMMU_CB_TLBIASID, smmu.context_number);
	smmu_wr(smmu.context + SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; (smmu_rd(smmu.context + SMMU_CB_TLBSTATUS) & 1) && ms < 100; ms++) {
		sleep_ms(1);
	}
}

static bool
smmu_on(void)
{
	uint64_t root;
	uint32_t sees = 1u << 31 | smmu.stream;

	if (kmem_alloc(kernel_map, &smmu.tables, round_page(TABLES * 4096), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		smmu.tables = 0;
		return false;
	}
	table(0)[IOVA_BASE >> 30 & 0x1ff] = kvtophys((vm_offset_t)table(1)) | 3;
	for (uint32_t i = 0; i < TABLES - 2; i++) {
		table(1)[(IOVA_BASE >> 21 & 0x1ff) + i] = kvtophys((vm_offset_t)table(2 + i)) | 3;
	}
	caches(smmu.tables, TABLES * 4096);
	root = kvtophys(smmu.tables) | (uint64_t)smmu.context_number << 48;
	tell("fastrpc: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets our page tables (root %#llx), match register %u sends "
	    "stream %#x through it (before: smr %#x, s2cr %#x)\n", smmu.context_number, root & 0xfffffffff000ull, smmu.match,
	    smmu.stream, smmu.old_smr, smmu.old_s2cr);
	smmu_wr(smmu.context + SMMU_CB_SCTLR, 0);
	smmu_wr(smmu.second + SMMU_CBA2R(smmu.context_number), SMMU_CBA2R_64_BIT);
	smmu_wr(smmu.second + SMMU_CBAR(smmu.context_number), SMMU_CBAR_STAGE_1);
	smmu_wr(smmu.context + SMMU_CB_TCR2, SMMU_TCR2);
	smmu_wr(smmu.context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	smmu_wr(smmu.context + SMMU_CB_TTBR0, (uint32_t)root);
	smmu_wr(smmu.context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	smmu_wr(smmu.context + SMMU_CB_TTBR1, 0);
	smmu_wr(smmu.context + SMMU_CB_TTBR1 + 4, 0);
	smmu_wr(smmu.context + SMMU_CB_MAIR0, SMMU_MAIR0);
	smmu_wr(smmu.context + SMMU_CB_MAIR1, 0);
	smmu_wr(smmu.context + SMMU_CB_FSR, SMMU_FSR_ALL);
	smmu_tlb();
	smmu_wr(smmu.context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	smmu.on = true;
	smmu_wr(smmu.global + SMMU_S2CR(smmu.match), smmu.context_number);
	smmu_wr(smmu.global + SMMU_SMR(smmu.match), sees);
	if (smmu_rd(smmu.global + SMMU_SMR(smmu.match)) != sees ||
	    (smmu_rd(smmu.global + SMMU_S2CR(smmu.match)) & 0x300ff) != smmu.context_number ||
	    !(smmu_rd(smmu.context + SMMU_CB_SCTLR) & 1) || smmu_rd(smmu.context + SMMU_CB_TTBR0) != (uint32_t)root) {
		tell("fastrpc: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x does not go through context %u\n", smmu.stream,
		    smmu.context_number);
		return false;
	}
	tell("fastrpc: stream %#x goes through context %u of the SMMU, with our page tables\n", smmu.stream, smmu.context_number);
	return true;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu.on) {
		return;
	}
	status = smmu_rd(smmu.context + SMMU_CB_FSR);
	global = smmu_rd(smmu.global + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		tell("fastrpc: the SMMU has refused the DSP nothing (context %u)\n", smmu.context_number);
		return;
	}
	tell("fastrpc: THE SMMU HAS REFUSED THE DSP: context %u: status %#x, address %#llx, syndrome %#x; global %#x, stream %#x\n",
	    smmu.context_number, status, (uint64_t)smmu_rd(smmu.context + SMMU_CB_FAR + 4) << 32 |
	    smmu_rd(smmu.context + SMMU_CB_FAR), smmu_rd(smmu.context + SMMU_CB_FSYNR0), global,
	    smmu_rd(smmu.global + SMMU_SGFSYNR1));
}

/* As found: the stream goes nowhere again, the context translates nothing. No lock, no sleep (the panic path too). */
static void
smmu_off(void)
{
	if (!smmu.on) {
		return;
	}
	smmu_wr(smmu.global + SMMU_SMR(smmu.match), smmu.old_smr);
	smmu_wr(smmu.global + SMMU_S2CR(smmu.match), smmu.old_s2cr);
	smmu_wr(smmu.context + SMMU_CB_SCTLR, smmu.old_sctlr);
	smmu.on = false;
}

/* ---- memory the DSP sees ---------------------------------------------------------------------------------------- */

static struct {
	vm_offset_t payload;            /* PAYLOAD_BYTES at IOVA_BASE */
	uint64_t next;                  /* the first IOVA not handed out to apps_mem */
	uint64_t sid_bits;              /* sid << 32: what the DSP's addresses carry (adsprpc.c: smmu.cb << 32) */
	uint8_t *in_pack, *out_pack;    /* PACK_BYTES each: next2's request, and our answer */
} mem;

static bool
mem_take(void)
{
	if (kmem_alloc(kernel_map, &mem.payload, PAYLOAD_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		mem.payload = 0;
		return false;
	}
	mem.in_pack = kalloc_data(PACK_BYTES, Z_WAITOK | Z_ZERO);
	mem.out_pack = kalloc_data(PACK_BYTES, Z_WAITOK | Z_ZERO);
	if (mem.in_pack == NULL || mem.out_pack == NULL) {
		return false;
	}
	smmu_enter(IOVA_BASE, mem.payload, PAYLOAD_BYTES);
	mem.next = IOVA_BASE + PAYLOAD_BYTES;
	mem.sid_bits = (uint64_t)sid << 32;
	return true;
}

/* ---- calls into the DSP ----------------------------------------------------------------------------------------- */

static LCK_MTX_DECLARE(call_lock, &fastrpc_lock_group);
static struct {
	volatile uint32_t waiting;      /* the number of the call in flight; 0: none */
	volatile bool answered;
	struct frpc_rsp rsp;
	uint32_t number, strays, sent, received;
} call;

static volatile bool wanted, runs;

/* From hakios.adsp.c's link thread: a response (struct fastrpc_invoke_rsp, or the vendor's v2). Only copied. */
void
dlisa_fastrpc_input(const uint8_t *data, uint32_t length)
{
	struct frpc_rsp r;

	call.received++;
	if (!frpc_rsp_parse(data, length, &r) || frpc_ctx_number(r.ctx) != call.waiting || call.waiting == 0 ||
	    !frpc_rsp_done(&r)) {
		call.strays++;
		return;
	}
	lck_mtx_lock(&call_lock);
	call.rsp = r;
	os_atomic_store(&call.answered, true, release);
	lck_mtx_unlock(&call_lock);
}

#define FOREVER                 0u

/*
 * One invoke: the payload laid out in our memory, the message sent, the response waited for (`ms`, or FOREVER while
 * this is wanted: next2 is answered only when the DSP has a call for us). 0 and the DSP's result in *result, or an errno.
 */
static int
invoke(uint32_t handle, uint32_t sc, struct frpc_buf *bufs, int32_t pid, uint32_t ms, int32_t *result)
{
	struct frpc_msg m;
	uint32_t size = frpc_payload_build((uint8_t *)mem.payload, PAYLOAD_BYTES, IOVA_BASE | mem.sid_bits, IOVA_BASE, sc, bufs);
	int error;

	if (size == 0) {
		return EMSGSIZE;
	}
	caches(mem.payload, size);
	call.number = call.number + 1 == 0 ? 1 : call.number + 1;
	os_atomic_store(&call.answered, false, release);
	call.waiting = call.number;
	frpc_msg_make(&m, pid, OUR_TID, call.number, PD_SENSORS, handle, sc, IOVA_BASE | mem.sid_bits, size);
	if ((error = dlisa_adsp_fastrpc_send(&m, sizeof(m))) != 0) {
		call.waiting = 0;
		return error;
	}
	call.sent++;
	for (uint32_t waited = 0; !os_atomic_load(&call.answered, acquire); waited++) {
		if ((ms != FOREVER && waited >= ms) || !wanted) {
			call.waiting = 0;
			return ETIMEDOUT;
		}
		sleep_ms(1);
	}
	call.waiting = 0;
	caches(mem.payload, size);
	frpc_payload_outputs((const uint8_t *)mem.payload, sc, bufs);
	*result = call.rsp.retval;
	return 0;
}

/* A call that must answer within 10 s and say 0; false, said in the log, otherwise. */
static bool
must(const char *what, uint32_t handle, uint32_t sc, struct frpc_buf *bufs, int32_t pid)
{
	int32_t result = -1;
	int error = invoke(handle, sc, bufs, pid, 10000, &result);

	if (error != 0 || result != 0) {
		tell("fastrpc: %s: %s %d\n", what, error ? "NOT ANSWERED, error" : "REFUSED by the DSP, result", error ? error : result);
		state("%s failed (%d/%d)", what, error, result);
		return false;
	}
	return true;
}

/* ---- apps_mem: pages of ours for the DSP's heap (Linux's fastrpc_req_mmap()) ------------------------------------ */

static uint32_t
map_heap(__unused void *ctx, uint64_t length, uint32_t flags, uint64_t *dsp_address)
{
	struct {
		int32_t client;
		uint32_t flags;
		uint64_t vaddr;
		int32_t num;
		uint32_t pad;
	} request = { OUR_TGID, flags, 0, 16, 0 };
	struct {
		uint64_t addr, size;
	} page;
	uint64_t answer = 0, size = (length + 16383) & ~16383ull;
	vm_offset_t memory;
	struct frpc_buf bufs[3] = { { &request, 24 }, { &page, sizeof(page) }, { &answer, sizeof(answer) } };
	int32_t result = -1;

	if (size == 0 || mem.next + size > IOVA_BASE + WINDOW_BYTES) {
		tell("fastrpc: apps_mem: the DSP asks for %llu bytes more; no room left in our window\n", length);
		return FRPC_AEE_ENOMEMORY;
	}
	if (kmem_alloc(kernel_map, &memory, (vm_size_t)size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		return FRPC_AEE_ENOMEMORY;
	}
	caches(memory, (uint32_t)size);
	smmu_enter(mem.next, memory, (uint32_t)size);
	smmu_tlb();
	page.addr = mem.next | mem.sid_bits;
	page.size = size;
	if (invoke(HANDLE_INIT, FRPC_SC(RMID_INIT_MMAP, 2, 1), bufs, 0, 10000, &result) != 0 || result != 0) {
		tell("fastrpc: apps_mem: the DSP did not take %llu KB of pages (result %d); they stay mapped, unused\n", size >> 10,
		    result);
		mem.next += size;
		return FRPC_AEE_EFAILED;
	}
	tell("fastrpc: apps_mem: %llu KB of ours at %#llx are the DSP's heap at %#llx\n", size >> 10, mem.next, answer);
	mem.next += size;
	*dsp_address = answer;
	return 0;
}

/* ---- the server and the listener ----------------------------------------------------------------------------- */

static struct frpc_server server;

static void *
server_alloc(__unused void *ctx, uint32_t size)
{
	return kalloc_data(size == 0 ? 1 : size, Z_WAITOK | Z_ZERO);
}

static void
server_free(__unused void *ctx, void *p, uint32_t size)
{
	kfree_data(p, size == 0 ? 1 : size);
}

static void
server_log(__unused void *ctx, const char *line)
{
	tell("fastrpc: %s\n", line);
}

static const struct frpc_ops server_ops = { server_alloc, server_free, server_log, map_heap, NULL };

/* remotectl open(in string name, rout long handle, rout sequence<char> dlerror, rout long nErr) on the DSP's side */
static bool
remote_open(const char *name, uint32_t *handle)
{
	uint32_t prim_in[2] = { (uint32_t)strlen(name) + 1, 256 }, prim_out[2] = { 0, 0 };
	static char dlerror[256];
	struct frpc_buf bufs[4] = { { prim_in, 8 }, { __DECONST(char *, name), prim_in[0] }, { prim_out, 8 }, { dlerror, 256 } };
	int32_t result = -1;
	int error = invoke(HANDLE_REMOTECTL, FRPC_SC(0, 2, 2), bufs, OUR_TGID, 10000, &result);

	if (error != 0 || result != 0 || prim_out[1] != 0) {
		dlerror[255] = 0;
		tell("fastrpc: the DSP does not open \"%s\": error %d, result %d, nErr %d, \"%s\"\n", name, error, result,
		    (int32_t)prim_out[1], dlerror);
		return false;
	}
	*handle = prim_out[0];
	return true;
}

static void
remote_close(uint32_t handle)
{
	uint32_t prim_in[2] = { handle, 256 }, prim_out = 0;
	static char text[256];
	struct frpc_buf bufs[3] = { { prim_in, 8 }, { &prim_out, 4 }, { text, 256 } };
	int32_t result;

	invoke(HANDLE_REMOTECTL, FRPC_SC(1, 1, 2), bufs, OUR_TGID, 10000, &result);
}

/*
 * next2(in ctx prevCtx, in long prevResult, in sequence<uint8> prevbufs, rout ctx, rout handle, rout uint32 sc,
 *       rout sequence<uint8> bufs, rout long bufsLenReq): our answer to the last call goes in, the next call comes out.
 * The request's packed buffers land in mem.in_pack (more than NEXT2_BYTES come through get_in_bufs2).
 */
static bool
next2(uint32_t *context, uint32_t result, uint32_t answer_size, uint32_t *handle, uint32_t *sc, uint32_t *length)
{
	uint32_t prim_in[4] = { *context, result, answer_size, NEXT2_BYTES }, prim_out[4] = { 0, 0, 0, 0 };
	struct frpc_buf bufs[4] = { { prim_in, 16 }, { mem.out_pack, answer_size }, { prim_out, 16 }, { mem.in_pack, NEXT2_BYTES } };
	int32_t r = -1;
	int error = invoke(HANDLE_LISTENER, FRPC_SC(LISTENER_NEXT2, 2, 2), bufs, OUR_TGID, FOREVER, &r);

	if (error != 0 || r != 0) {
		tell("fastrpc: next2: %s %d\n", error ? "not answered, error" : "the DSP says", error ? error : r);
		return false;
	}
	*context = prim_out[0];
	*handle = prim_out[1];
	*sc = prim_out[2];
	*length = prim_out[3];
	if (*length <= NEXT2_BYTES) {
		return true;
	}
	if (*length > PACK_BYTES) {
		tell("fastrpc: a call of %u bytes of input: more than the %u taken here\n", *length, PACK_BYTES);
		return false;
	}
	{
		/* get_in_bufs2(in ctx, in long offset, rout sequence<uint8> bufs, rout long bufsLenReq) */
		uint32_t more_in[3] = { *context, NEXT2_BYTES, *length - NEXT2_BYTES }, more_out = 0;
		struct frpc_buf more[3] = { { more_in, 12 }, { &more_out, 4 }, { mem.in_pack + NEXT2_BYTES, *length - NEXT2_BYTES } };

		error = invoke(HANDLE_LISTENER, FRPC_SC(LISTENER_GET_IN_BUFS2, 1, 2), more, OUR_TGID, 10000, &r);
		if (error != 0 || r != 0) {
			tell("fastrpc: get_in_bufs2 of %u bytes: error %d, result %d\n", *length, error, r);
			return false;
		}
	}
	return true;
}

/* One reverse call served: the DSP's buffers unpacked, ours laid out in mem.out_pack, the server called. */
static uint32_t
serve(uint32_t handle, uint32_t sc, uint32_t length, uint32_t *answer_size)
{
	struct frpc_view in[FRPC_MAX_BUFS];
	struct frpc_buf out[FRPC_MAX_BUFS];
	uint32_t lengths[FRPC_MAX_BUFS], result;

	*answer_size = 0;
	if (!frpc_unpack_in(mem.in_pack, length, sc, in, lengths)) {
		tell("fastrpc: a call (handle %u, sc %#x) whose %u bytes do not unpack\n", handle, sc, length);
		return FRPC_AEE_EBADPARM;
	}
	*answer_size = frpc_pack_out_layout(mem.out_pack, PACK_BYTES, FRPC_SC_OUTBUFS(sc), lengths, out);
	if (*answer_size == 0 && FRPC_SC_OUTBUFS(sc) != 0) {
		tell("fastrpc: a call (handle %u, sc %#x) wants more answer than the %u bytes here\n", handle, sc, PACK_BYTES);
		return FRPC_AEE_ENOMEMORY;
	}
	result = frpc_server_call(&server, handle, sc, in, out);
	if (result == FRPC_AEE_EUNSUPPORTED && server.calls < 4000) {
		tell("fastrpc: the DSP calls handle %u method %u (sc %#x): not served here\n", handle, FRPC_SC_METHOD(sc), sc);
	}
	*answer_size = result == 0 ? *answer_size : 0;  /* (listener_android.c: no answer's buffers with an error) */
	return result;
}

static void
listener_loop(void)
{
	uint32_t context = 0, result = 0xffffffffu, answer_size = 0, failures = 0;

	while (wanted) {
		uint32_t handle = 0, sc = 0, length = 0;

		if (!next2(&context, result, answer_size, &handle, &sc, &length)) {
			if (++failures == 3 || !wanted) {
				break;
			}
			context = 0;
			result = 0xffffffffu;
			answer_size = 0;
			sleep_ms(10);
			continue;
		}
		failures = 0;
		result = serve(handle, sc, length, &answer_size);
		if ((server.calls & 63) == 1) {
			state("serving: %u calls, %u KB written by the DSP (in RAM)", server.calls, server.overlay_bytes >> 10);
		}
	}
	tell("fastrpc: the listener stops after %u calls of the DSP's (%u stray messages)\n", server.calls, call.strays);
}

/* ---- the thread --------------------------------------------------------------------------------------------- */

static const uint8_t *
archive(uint32_t *size)
{
#ifdef DLISA_SENSORS_FS
	*size = DLISA_SENSORS_FS_SIZE;
	return dlisa_sensors_fs;
#else
	*size = 0;
	return NULL;
#endif
}

/*
 * Attach to the sensors PD, open and register the default listener, init2 it: what sscrpcd does through
 * libadsp_default_listener.so, and hexagonrpcd -s. False if a step is refused (the PD may not be up yet: try again).
 */
static bool
attach_listener(void)
{
	uint32_t listener = 0, tgid = OUR_TGID;
	struct frpc_buf attach[1] = { { &tgid, 4 } };

	if (!must("attach to the sensors PD (INIT_ATTACH, PD 2)", HANDLE_INIT, FRPC_SC(RMID_INIT_ATTACH, 1, 0), attach, 0)) {
		return false;
	}
	tell("fastrpc: ATTACHED TO THE SENSORS PD (stream %#x, our process %#x)\n", smmu.stream, OUR_TGID);
	if (!remote_open("adsp_default_listener", &listener)) {
		state("adsp_default_listener did not open");
		return false;
	}
	if (!must("adsp_default_listener register", listener, FRPC_SC(0, 0, 0), NULL, OUR_TGID)) {
		return false;
	}
	remote_close(listener);
	return must("the listener's init2", HANDLE_LISTENER, FRPC_SC(LISTENER_INIT2, 0, 0), NULL, OUR_TGID);
}

static void
rpc_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t wait_seconds = 60, tries = 30, size;
	const uint8_t *files = archive(&size);
	bool smmu_ok, attached = false;

	runs = true;
	PE_parse_boot_argn("sensors_rpc_wait", &wait_seconds, sizeof(wait_seconds));
	PE_parse_boot_argn("sensors_rpc_tries", &tries, sizeof(tries));
	if (!frpc_server_init(&server, &server_ops, files, size)) {
		tell("fastrpc: NO SENSOR FILES in this kernel (sensors-wire.py builds them in): the sensors PD is not served\n");
		state("no sensor files built in");
		goto out;
	}
	server.trace = trace_budget;
	tell("fastrpc: %u sensor files (%u KB) to serve; waiting for the DSP's channel \"fastrpcglink-apps-dsp\"\n", server.count,
	    size >> 10);
	for (uint32_t ms = 0; wanted && !dlisa_adsp_fastrpc_open(); ms += 100) {
		if (ms >= wait_seconds * 1000) {
			tell("fastrpc: THE DSP HAS NOT OPENED \"fastrpcglink-apps-dsp\" in %u s\n", wait_seconds);
			state("the channel never opened");
			goto out;
		}
		sleep_ms(100);
	}
	lck_mtx_lock(&hakios_smmu_lock);
	smmu.stream = 0x1800 | sid;
	smmu_ok = smmu_find() && smmu_on();
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!smmu_ok || !mem_take()) {
		state("no SMMU context or no memory for stream %#x", smmu.stream);
		goto out;
	}
	/* (sscrpcd starts again 25 ms after a failure; the sensors PD may come up after the channel) */
	for (uint32_t i = 0; i < tries && wanted && !attached; i++) {
		state("attaching to the sensors PD, try %u", i + 1);
		attached = attach_listener();
		if (!attached) {
			smmu_faults();
			sleep_ms(2000);
		}
	}
	if (!attached) {
		tell("fastrpc: THE SENSORS PD WAS NOT ATTACHED in %u tries\n", tries);
		goto out;
	}
	tell("fastrpc: THE SENSORS PD'S LISTENER IS UP: its calls for files are answered from here on\n");
	state("listening");
	listener_loop();
	smmu_faults();
	state("stopped after %u calls; %u KB written by the DSP (in RAM)", server.calls, server.overlay_bytes >> 10);
out:
	runs = false;
	thread_terminate(current_thread());
}

bool
dlisa_fastrpc_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("sensors_rpc", &on, sizeof(on)) && on != 0;
}

void
dlisa_fastrpc_start(void)
{
	thread_t thread;

	if (!dlisa_fastrpc_wanted() || runs) {
		return;
	}
	PE_parse_boot_argn("sensors_rpc_trace", &trace_budget, sizeof(trace_budget));
	PE_parse_boot_argn("sensors_rpc_sid", &sid, sizeof(sid));
	sid = sid >= 3 && sid <= 5 ? sid : 5;
	wanted = true;
	state("starting");
	if (kernel_thread_start(rpc_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
dlisa_fastrpc_stop(void)
{
	wanted = false;
	for (unsigned ms = 0; ms < 2000 && runs; ms++) {
		sleep_ms(1);
	}
	lck_mtx_lock(&hakios_smmu_lock);
	smmu_faults();
	smmu_off();
	lck_mtx_unlock(&hakios_smmu_lock);
}

void
dlisa_fastrpc_quiesce(void)
{
	wanted = false;
	smmu_off();
}
