#!/usr/bin/env python3
# darwin-lisa (the UniXNU stack, T2): the GPU init that Linux does for this chip and our driver leaves at the reset values,
# on by default since t3b (2026-11-16: the home and the Control Center right, 60 fps in the lock-to-home gesture); boot-arg
# gpu_hwcg=0 leaves it out (the reset values, as before).
#
#   bsd/dev/hakios.adreno.c   after RB_DBG_ECO_CNTL, unless gpu_hwcg=0:
#     - the hardware clock gating, Linux's a6xx_set_hwcg() with the A635's table (a660_hwcg in drivers/gpu/drm/msm/adreno/
#       a6xx_catalog.c; unixnu-adreno6xx's NativeAdrenoA6xxHwInit.cpp has the same 49 values for the A642L): the GMU's CGC
#       mode 0x00020202, delay 0x10111, hysteresis 0x5555; the SP clock off while the table is written; RBBM_CLOCK_CNTL
#       0x8aa8aa82;
#     - the bicubic weights Linux writes for the a650 family (TPL1_BICUBIC_WEIGHTS_TABLE).
#   CP protection (Linux's a660_protect) is left out: it is no candidate for the bands and
#   could fault on a register a command buffer writes.
#
# Run in the toolchain container like the other wires (build-kernel.sh does). Idempotent. XNU_WORK=<dir> for another tree.
import os
import sys

T = os.environ.get("XNU_WORK", "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")
ADRENO = os.path.join(T, "bsd/dev/hakios.adreno.c")

# Linux a660_hwcg (register offset in words, value), in its order
HWCG = [
    (0xb0, 0x02222222), (0xb4, 0x02222220), (0xb8, 0x00000080), (0xbc, 0x0000f3cf), (0xc0, 0x22222222), (0xc4, 0x22222222),
    (0xc8, 0x22222222), (0xcc, 0x00022222), (0xd0, 0x11111111), (0xd4, 0x11111111), (0xd8, 0x11111111), (0xdc, 0x00011111),
    (0xe0, 0x77777777), (0xe4, 0x77777777), (0xe8, 0x77777777), (0xec, 0x00077777), (0xf0, 0x22222222), (0xf4, 0x01002222),
    (0xf8, 0x00002220), (0x100, 0x00040f00), (0x104, 0x25222022), (0x105, 0x00005555), (0x106, 0x00000011),
    (0x107, 0x00445044), (0x108, 0x04222222), (0x111, 0x00002222), (0x114, 0x00222222), (0x117, 0x00000002),
    (0x11b, 0x00002222), (0x109, 0x00004000), (0x112, 0x00002222), (0x115, 0x00000200), (0x11c, 0x00000000),
    (0x10a, 0x00000000), (0x113, 0x00000000), (0x116, 0x04104004), (0x11d, 0x00000000), (0x120, 0x00000222),
    (0x121, 0x00000111), (0x122, 0x00000000), (0x10b, 0x22222222), (0x110, 0x00000004), (0x10f, 0x00000002),
    (0x533, 0x00000182), (0x44, 0x00000000), (0x42, 0x00000000), (0x118, 0x00000222), (0x119, 0x00000111),
    (0x11a, 0x00000555),
]
BICUBIC = [0, 0x3fe05ff4, 0x3fa0ebee, 0x3f5193ed, 0x3f0243f0]

ANCHOR = '\tput(&gpu, "rb_dbg_eco_cntl", RB_DBG_ECO_CNTL, 0x04100000);     /* Mesa\'s value for this GPU */\n'
MARK = "darwin-lisa (kernel/drivers/gpu/unixnu/kernel-wire.py): boot-arg gpu_hwcg=1"

rows = "\n".join("\t\t\t\t{ %#05x, 0x%08x }," % r for r in HWCG)
BLOCK = f'''	/*
	 * {MARK} (default, gpu_hwcg=0 leaves it out): the hardware clock gating Linux programs for
	 * this GPU (a6xx_set_hwcg with the A635's a660_hwcg; the GMU's CGC mode, delay and hysteresis; the SP clock off while the
	 * table is written) and the bicubic weights of the a650 family, which this driver otherwise leaves at their reset values.
	 */
	{{
		uint32_t hwcg = 1;

		PE_parse_boot_argn("gpu_hwcg", &hwcg, sizeof(hwcg));
		if (hwcg) {{
			static const uint32_t table[][2] = {{
{rows}
			}};
			static const uint32_t bicubic[] = {{ {", ".join("0x%08x" % v for v in BICUBIC)} }};

			put(&gmu, "gmu_ao_cgc_mode_cntl", GMU_WORD(0x9309), 0x00020202);
			put(&gmu, "gmu_ao_cgc_delay_cntl", GMU_WORD(0x930a), 0x00010111);
			put(&gmu, "gmu_ao_cgc_hyst_cntl", GMU_WORD(0x930b), 0x00005555);
			put(&gmu, "gx_sptprac_clock_control (sp clock off)", GMU_WORD(0x80), rd(gmu.base + GMU_WORD(0x80)) & ~1u);
			for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {{
				put(&gpu, "hwcg", GPU_WORD(table[i][0]), table[i][1]);
			}}
			put(&gmu, "gx_sptprac_clock_control (sp clock on)", GMU_WORD(0x80), rd(gmu.base + GMU_WORD(0x80)) | 1u);
			put(&gpu, "rbbm_clock_cntl", RBBM_CLOCK_CNTL, 0x8aa8aa82);
			for (unsigned i = 0; i < sizeof(bicubic) / sizeof(bicubic[0]); i++) {{
				put(&gpu, "tpl1_bicubic_weights_table", GPU_WORD(0xb608 + i), bicubic[i]);
			}}
			printf("adreno: gpu_hwcg=1: Linux's clock gating (%u registers) and bicubic weights written\\n",
			    (unsigned) (sizeof(table) / sizeof(table[0])));
		}}
	}}
'''

s = open(ADRENO).read()
OFF = "\t\tuint32_t hwcg = 0;\n\n\t\tPE_parse_boot_argn(\"gpu_hwcg\""
if MARK in s and OFF in s:
    # the first version (off unless gpu_hwcg=1): on by default now
    s = s.replace(OFF, OFF.replace("hwcg = 0;", "hwcg = 1;"))
    s = s.replace(f" * {MARK}: the hardware", f" * {MARK} (default, gpu_hwcg=0 leaves it out): the hardware")
    with open(ADRENO, "w") as f:
        f.write(s)
    print("  [ok]   hakios.adreno.c: gpu_hwcg on by default")
elif MARK in s:
    print("  [skip] hakios.adreno.c: gpu_hwcg already wired")
elif s.count(ANCHOR) != 1:
    print(f"  [FAIL] hakios.adreno.c: anchor found {s.count(ANCHOR)} times (need 1)")
    sys.exit(2)
else:
    for need in ("RBBM_CLOCK_CNTL", "GMU_WORD(", "GPU_WORD(", "PE_parse_boot_argn"):
        if need not in s:
            print(f"  [FAIL] hakios.adreno.c: no {need}")
            sys.exit(2)
    with open(ADRENO, "w") as f:
        f.write(s.replace(ANCHOR, ANCHOR + BLOCK))
    print("  [ok]   hakios.adreno.c: Linux's clock gating and bicubic weights (default; gpu_hwcg=0 leaves them out)")

# ---- boot-arg gpu_irq=1: the GPU's interrupt wakes the waiters of a fence (B1n), instead of a look every 200 us ----
#   osfmk/arm64/gic_vtimer.c  kernel/soc/gic_vtimer.c (the copy apply.sh makes) with gic_spi_handler_set(): a table of
#                             shared peripheral interrupts and their handlers, called between acknowledge and end
#   bsd/dev/hakios.adreno.c   gpu_irq_start() when the GPU is kept: the distributor set up for SPI 300 (intid 332, level;
#                             dt: gpu@3d00000), RBBM_INT_0_MASK = CP_CACHE_FLUSH_TS, the handler clears RBBM and wakes
#                             hakios_gpu_irq()'s event; every submission's fence event raises the interrupt; gpu_irq_stop()
#                             before the GPU is given back and when it is quiesced. hakios.gpu.cpp's gpuWait() sleeps on
#                             the event (1 ms at most, then it looks again) while the line is up.
import shutil
GIC_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "soc", "gic_vtimer.c")
GIC_DST = os.path.join(T, "osfmk/arm64/gic_vtimer.c")
if open(GIC_SRC).read() == open(GIC_DST).read():
    print("  [skip] gic_vtimer.c: the same")
else:
    if "gic_spi_handler_set" not in open(GIC_SRC).read():
        print("  [FAIL] kernel/soc/gic_vtimer.c has no gic_spi_handler_set")
        sys.exit(2)
    shutil.copyfile(GIC_SRC, GIC_DST)
    print("  [ok]   copied osfmk/arm64/gic_vtimer.c (gic_spi_handler_set)")

IRQ_MARK = "darwin-lisa (kernel/drivers/gpu/unixnu/kernel-wire.py): boot-arg gpu_irq=1"
IRQ_CODE = f"""/*
 * {IRQ_MARK}: the GPU's interrupt (SPI 300, level, high: dt gpu@3d00000) wakes whoever waits for a fence (hakios.gpu.cpp's
 * gpuWait()), instead of a look every 200 us. Every submission's CACHE_FLUSH_TS raises it (RBBM_INT_0 bit 20); the handler
 * clears RBBM's status and wakes the event. The distributor's line is set up here (group 1, level, routed to this processor)
 * and given back before the GPU is (gpu_irq_stop()), so the loader finds it as it left it.
 */
#define GPU_IRQ_INTID           332u
#define GICD_PHYS               0x17a00000u
#define GICD_SIZE               0x10000u
#define RBBM_INT_CLEAR_CMD      GPU_WORD(0x0037)
#define RBBM_INT_0_MASK         GPU_WORD(0x0038)
#define RBBM_INT_CP_CACHE_FLUSH_TS (1u << 20)
#define EVENT_WRITE_IRQ         (1u << 31)
extern void gic_spi_handler_set(uint32_t intid, void (*handler)(void *), void *arg);
static uint32_t gpu_irq;                /* the line is up */
static vm_offset_t gicd;
static uint64_t gpu_irqs;
static int gpu_fence_event;

static void
gpu_irq_handler(void *arg __unused)
{{
	uint32_t status = rd(gpu.base + RBBM_INT_0_STATUS);

	wr(gpu.base + RBBM_INT_CLEAR_CMD, status);
	gpu_irqs++;
	wakeup(&gpu_fence_event);
}}

static void
gpu_irq_start(void)
{{
	uint32_t want = 0, word = GPU_IRQ_INTID / 32, bit = 1u << (GPU_IRQ_INTID % 32), shift = 2 * (GPU_IRQ_INTID % 16);
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");

	if (!PE_parse_boot_argn("gpu_irq", &want, sizeof(want)) || !want) {{
		return;
	}}
	gicd = gicd != 0 ? gicd : ml_io_map(GICD_PHYS, GICD_SIZE);
	if (gicd == 0) {{
		printf("adreno: gpu_irq=1: the interrupt distributor is not mapped; fences are looked at as before\\n");
		return;
	}}
	wr(gicd + 0x180 + 4 * word, bit);                                       /* GICD_ICENABLER: off while set up */
	wr(gicd + 0x080 + 4 * word, rd(gicd + 0x080 + 4 * word) | bit);         /* GICD_IGROUPR: group 1 */
	wr(gicd + 0xd00 + 4 * word, rd(gicd + 0xd00 + 4 * word) & ~bit);        /* GICD_IGRPMODR: non-secure group 1 */
	*(volatile uint8_t *)(gicd + 0x400 + GPU_IRQ_INTID) = 0xa0;             /* GICD_IPRIORITYR */
	wr(gicd + 0xc00 + 4 * (GPU_IRQ_INTID / 16), rd(gicd + 0xc00 + 4 * (GPU_IRQ_INTID / 16)) & ~(2u << shift)); /* level */
	*(volatile uint64_t *)(gicd + 0x6000 + 8 * GPU_IRQ_INTID) = (mpidr & 0xffffffull) | ((mpidr >> 32 & 0xffull) << 32);
	gic_spi_handler_set(GPU_IRQ_INTID, gpu_irq_handler, NULL);
	wr(gpu.base + RBBM_INT_CLEAR_CMD, 0xffffffffu);
	wr(gpu.base + RBBM_INT_0_MASK, RBBM_INT_CP_CACHE_FLUSH_TS);
	gpu_irq = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(gicd + 0x100 + 4 * word, bit);                                       /* GICD_ISENABLER */
	printf("adreno: gpu_irq=1: the GPU's interrupt (intid %u, to processor %#llx) wakes the fences' waiters\\n",
	    GPU_IRQ_INTID, mpidr & 0xffffffull);
}}

static void
gpu_irq_stop(void)
{{
	uint32_t word = GPU_IRQ_INTID / 32, bit = 1u << (GPU_IRQ_INTID % 32);

	if (!gpu_irq) {{
		return;
	}}
	gpu_irq = 0;
	wr(gicd + 0x180 + 4 * word, bit);
	wr(gpu.base + RBBM_INT_0_MASK, 0);
	wr(gpu.base + RBBM_INT_CLEAR_CMD, 0xffffffffu);
	gic_spi_handler_set(GPU_IRQ_INTID, NULL, NULL);
	printf("adreno: gpu_irq: %llu interrupts; the line given back\\n", gpu_irqs);
}}

/* For hakios.gpu.cpp: whether the interrupt wakes fences' waiters, and the event they sleep on. */
bool hakios_gpu_irq(void **event);
bool
hakios_gpu_irq(void **event)
{{
	if (event != NULL) {{
		*event = &gpu_fence_event;
	}}
	return gpu_irq != 0;
}}

"""
s = open(ADRENO).read()
if IRQ_MARK in s:
    print("  [skip] hakios.adreno.c: gpu_irq already wired")
else:
    edits = [
        ("void\nhakios_gpu_stop(void)\n{\n\tif (gpu_kept) {\n", IRQ_CODE + "void\nhakios_gpu_stop(void)\n{\n\tgpu_irq_stop();\n\tif (gpu_kept) {\n"),
        ("void\nhakios_adreno_quiesce(void)\n{\n", "void\nhakios_adreno_quiesce(void)\n{\n\tgpu_irq_stop();\n"),
        ("\tring(EVENT_CACHE_FLUSH);\n\tring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);\n\tring(0);\n\tring(gpu_fence);\n\tring_send();\n\treturn gpu_fence;\n",
         "\tring(EVENT_CACHE_FLUSH | (gpu_irq ? EVENT_WRITE_IRQ : 0));\n\tring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);\n\tring(0);\n\tring(gpu_fence);\n\tring_send();\n\treturn gpu_fence;\n"),
        ("\t\t\tprintf(\"adreno: THE GPU STAYS ON for the system to draw with: the service HakiosGPU\\n\");\n\t\t\tHakiosGPUStart();\n",
         "\t\t\tprintf(\"adreno: THE GPU STAYS ON for the system to draw with: the service HakiosGPU\\n\");\n\t\t\tgpu_irq_start();\n\t\t\tHakiosGPUStart();\n"),
    ]
    for old, new in edits:
        if s.count(old) != 1:
            print(f"  [FAIL] hakios.adreno.c: gpu_irq anchor found {s.count(old)} times: {old[:60]!r}")
            sys.exit(2)
    # the quiesce function comes before hakios_gpu_stop: the code must be above both
    q = s.index("void\nhakios_adreno_quiesce(void)\n{\n")
    s = s[:q] + IRQ_CODE + s[q:]
    for old, new in edits:
        new = new.replace(IRQ_CODE, "")
        s = s.replace(old, new)
    with open(ADRENO, "w") as f:
        f.write(s)
    print("  [ok]   hakios.adreno.c: boot-arg gpu_irq=1 (the GPU's interrupt wakes fences' waiters)")

print("### UNIXNU KERNEL WIRE DONE")
