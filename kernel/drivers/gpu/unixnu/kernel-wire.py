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
print("### UNIXNU KERNEL WIRE DONE")
