#!/usr/bin/env python3
# darwin-lisa: wire HakiOS's Wi-Fi driver for the WCN6750 (the Wi-Fi half of the Snapdragon
# SM7325 "lisa") into the xnu-12377 work tree as the OPTIONAL driver tag `hakios_wifi`. The
# standard input driver (hakios.input.c) already chains hakios_wifi_init(); enabling the tag
# swaps the hakios.stubs.c no-ops (#ifndef HAKIOS_WIFI: hakios_wifi_init/stop/quiesce,
# hakios_apple80211_ioctl, HakiosWLANStart) for the real driver. Idempotent; run in docker (root).
# Models goodix-wire.py / adreno-wire.py.
#
# The bsd side is the driver proper: hakios.wifi.c (the DSP + GLINK/QRTR bring-up stepper),
# hakios.qmi.c (QMI over the IPC router -- WIFI `requires QMI` in HakiOS; the only in-kernel
# consumer we enable is wifi, so it rides the same tag here), hakios.wlfw.c (the firmware QMI
# exchange; see the deferred note below), hakios.lx.c (the ath11k-shaped net glue), hakios.sta.c
# and hakios.apple80211.c (the Apple80211 ioctl surface wifid talks to).
#
# The iokit side is LOW-churn registry glue (no IO80211Family/IONetworkController KPI -- the file
# declares its own minimal IOService subclasses), so unlike Adreno's "metal stack" it is ported
# for real: hakios.wlan.cpp (HakiosWLANStart -- the en0 controller/interface registry objects
# wifid + configd look for) and MiniIONetworkStack.cpp (the IONetworkStack stand-in configd's
# interface namer waits for). HakiOS starts MiniIONetworkStack from its own GenericARMPE; this
# tree keeps Apple's platform expert, so wlan.cpp calls MiniIONetworkStackStart() itself, at the
# one moment the radio's interface is published (so both stay inert until wifi=1).
import os, shutil, sys

H = "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/xnu-src/xnu-8796.121.2"
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"

def copy(src, dst, tag):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    print(f"  [ok]   copied {tag}")

def edit(path, old, new, tag):
    with open(path) as f:
        s = f.read()
    if new in s:
        print(f"  [skip] {tag}: already applied")
        return
    n = s.count(old)
    if n != 1:
        print(f"  [FAIL] {tag}: anchor found {n} times (need 1) in {path}")
        sys.exit(2)
    with open(path, "w") as f:
        f.write(s.replace(old, new))
    print(f"  [ok]   {tag}")

# 1. bsd driver sources (6: the 5 listed + hakios.qmi.c, which WIFI requires).
BSD_SRCS = [
    "hakios.wifi.c", "hakios.qmi.c", "hakios.wlfw.c",
    "hakios.lx.c", "hakios.sta.c", "hakios.apple80211.c",
]
for f in BSD_SRCS:
    copy(os.path.join(H, "bsd/dev", f), os.path.join(T, "bsd/dev", f), f"bsd/dev/{f}")

# 2. bsd/sys headers the driver #includes as <sys/hakios.*.h> and that are not yet in the tree.
#    (hakios.net.h and hakios.input.h are already present from the net/input port.)
for h in ["hakios.lx.h", "hakios.qmi.h", "hakios.qrtr.h", "hakios.sta.h"]:
    copy(os.path.join(H, "bsd/sys", h), os.path.join(T, "bsd/sys", h), f"bsd/sys/{h}")

# 3. iokit sources (the two cpp + the MiniDrivers.h MiniIONetworkStack.cpp includes).
copy(os.path.join(H, "iokit/Kernel/hakios.wlan.cpp"), os.path.join(T, "iokit/Kernel/hakios.wlan.cpp"), "iokit/Kernel/hakios.wlan.cpp")
copy(os.path.join(H, "iokit/Kernel/MiniIONetworkStack.cpp"), os.path.join(T, "iokit/Kernel/MiniIONetworkStack.cpp"), "iokit/Kernel/MiniIONetworkStack.cpp")
copy(os.path.join(H, "iokit/Kernel/MiniDrivers.h"), os.path.join(T, "iokit/Kernel/MiniDrivers.h"), "iokit/Kernel/MiniDrivers.h")

# 4. KPI rebase (xnu-8796 -> xnu-12377): the KMA_* flags moved out of <vm/vm_kern.h> into the
#    XNU-internal <vm/vm_kern_xnu.h> in 26 (same move adreno.c / kern_minimac.c needed). Only the
#    two sources that allocate with kmem_alloc/kmem_alloc_contig + KMA_* need it; qmi.c/sta.c/
#    apple80211.c use kalloc_data only, and wlfw.c compiles to its stub branch (see step 5).
for f in ["hakios.wifi.c", "hakios.lx.c"]:
    edit(os.path.join(T, "bsd/dev", f),
         "#include <vm/vm_kern.h>\n",
         "#include <vm/vm_kern.h>\n#include <vm/vm_kern_xnu.h>   /* darwin-lisa: KMA_* flags on xnu-12377 */\n",
         f"{f}: include vm_kern_xnu.h")

# 5. wlan.cpp: publish the IONetworkStack stand-in from here. HakiOS's GenericARMPE calls
#    MiniIONetworkStackStart() unconditionally; this tree keeps Apple's platform expert, so the
#    one wifi-gated call site is the interface publish. Keeps MiniIONetworkStack inert until wifi=1.
wlan = os.path.join(T, "iokit/Kernel/hakios.wlan.cpp")
edit(wlan,
     "#include <libkern/c++/OSNumber.h>\n",
     "#include <libkern/c++/OSNumber.h>\n#include \"MiniDrivers.h\"   /* darwin-lisa: MiniIONetworkStackStart */\n",
     "hakios.wlan.cpp: include MiniDrivers.h")
edit(wlan,
     "HakiosWLANStart(const char * interface, const uint8_t * address)\n{\n\tIOService * platform = IOService::getPlatform();\n",
     "HakiosWLANStart(const char * interface, const uint8_t * address)\n{\n"
     "\tMiniIONetworkStackStart();   /* darwin-lisa: the IONetworkStack configd's namer waits for (our GenericARMPE does not start it) */\n"
     "\tIOService * platform = IOService::getPlatform();\n",
     "hakios.wlan.cpp: call MiniIONetworkStackStart")

# 5b. GICv3-SPI no-op glue. hakios.lx.c defines lx_os_irq_* (the radio's hardware-interrupt path,
#     reached only under wifi_irq=1 and only from ath11k's lx_bus.c, which is NOT linked into this
#     kernel). Those bodies call gic_v3_spi_attach/mask/unmask/detach/count, which HakiOS provides
#     in osfmk/arm64/gic_v3_irq.c -- a full GICv3 SPI driver this tree does not carry (its GICv3
#     work is the vtimer/FIQ bring-up, which must not be touched). So the symbols are UNDEFINED at
#     link. These file-local stubs (correct signatures) satisfy the linker; they are never called
#     at runtime in this build (no ath11k => lx_os_irq_* is dead code). The default Wi-Fi path polls
#     the radio's rings, so nothing is lost. When the radio's SPI routing is ported (gic_v3_irq.c),
#     delete this file so its symbols do not clash with the real ones. Mirrors hakios.adreno_glue.c.
GLUE = r'''/*
 * darwin-lisa: file-local no-op stand-ins for the GICv3 SPI (shared peripheral interrupt) routing
 * that bsd/dev/hakios.lx.c's lx_os_irq_* helpers call but that is NOT ported to this xnu-12377 tree.
 * The real ones live in HakiOS osfmk/arm64/gic_v3_irq.c (a full GICv3 SPI driver); this tree carries
 * only the vtimer/FIQ GICv3 bring-up, which must not be touched.
 *
 * lx_os_irq_* is the radio's hardware-interrupt path (wifi_irq=1), reached only from ath11k's
 * lx_bus.c, which is not linked into this kernel (the ath11k object is absent). So these stubs are
 * never called at runtime: the default Wi-Fi path looks at the radio's rings every millisecond
 * (polling) instead. With them the Wi-Fi driver links and registers; with real SPI routing the
 * radio could raise MSIs through the GIC. When gic_v3_irq.c is ported, remove this file so its
 * symbols do not clash with the real ones.
 */
#include <stdbool.h>
#include <stdint.h>
#include <mach/kern_return.h>

kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);

kern_return_t
gic_v3_spi_attach(__unused uint32_t intid, __unused bool edge,
    __unused void (*handler)(void *context, uint32_t intid), __unused void *context)
{
	return KERN_FAILURE;            /* no SPI routing here: lx_os_irq_attach then reports EINVAL */
}

void
gic_v3_spi_mask(__unused uint32_t intid)
{
}

void
gic_v3_spi_unmask(__unused uint32_t intid)
{
}

void
gic_v3_spi_detach(__unused uint32_t intid)
{
}

uint64_t
gic_v3_spi_count(__unused uint32_t intid)
{
	return 0;                       /* "no interrupts seen" */
}
'''
glue_path = os.path.join(T, "bsd/dev/hakios.wifi_glue.c")
with open(glue_path, "w") as f:
    f.write(GLUE)
print("  [ok]   wrote bsd/dev/hakios.wifi_glue.c (GICv3-SPI stubs)")

# 6. bsd/conf/files: the 6 driver sources + the GICv3-SPI glue, built only with the hakios_wifi tag.
bf = os.path.join(T, "bsd/conf/files")
with open(bf) as f:
    bfc = f.read()
if "bsd/dev/hakios.wifi.c" in bfc:
    print("  [skip] bsd/conf/files: hakios.wifi already listed")
else:
    with open(bf, "a") as f:
        f.write("\n# HakiOS Wi-Fi driver for the WCN6750 (optional; ported from xnu-8796).\n"
                "# hakios.qmi.c is the QMI transport the driver requires; all ride the hakios_wifi tag.\n"
                "# hakios.wifi_glue.c carries no-op stand-ins for the not-yet-ported GICv3 SPI routing.\n"
                "bsd/dev/hakios.wifi.c\t\t\toptional hakios_wifi\n"
                "bsd/dev/hakios.qmi.c\t\t\toptional hakios_wifi\n"
                "bsd/dev/hakios.wlfw.c\t\t\toptional hakios_wifi\n"
                "bsd/dev/hakios.lx.c\t\t\toptional hakios_wifi\n"
                "bsd/dev/hakios.sta.c\t\t\toptional hakios_wifi\n"
                "bsd/dev/hakios.apple80211.c\t\toptional hakios_wifi\n"
                "bsd/dev/hakios.wifi_glue.c\t\toptional hakios_wifi\n")
    print("  [ok]   bsd/conf/files += 6 wifi sources + glue")

# 6b. ensure the glue line is present even if the 6 sources were listed by an earlier run.
with open(bf) as f:
    bfc = f.read()
if "bsd/dev/hakios.wifi_glue.c" not in bfc:
    with open(bf, "a") as f:
        f.write("bsd/dev/hakios.wifi_glue.c\t\toptional hakios_wifi\n")
    print("  [ok]   bsd/conf/files += hakios.wifi_glue.c")

# 7. iokit/conf/files: the two cpp (iokitcpp is in IOKIT_BASE, so active in this config).
iof = os.path.join(T, "iokit/conf/files")
with open(iof) as f:
    ifc = f.read()
if "iokit/Kernel/hakios.wlan.cpp" in ifc:
    print("  [skip] iokit/conf/files: hakios.wlan already listed")
else:
    with open(iof, "a") as f:
        f.write("\n# HakiOS Wi-Fi registry glue (optional; ported from xnu-8796).\n"
                "iokit/Kernel/hakios.wlan.cpp\t\toptional hakios_wifi iokitcpp\n"
                "iokit/Kernel/MiniIONetworkStack.cpp\toptional hakios_wifi iokitcpp\n")
    print("  [ok]   iokit/conf/files += hakios.wlan + MiniIONetworkStack")

# 8. config/MASTER: declare the HAKIOS_WIFI option behind the <hakios_wifi> tag (mirrors
#    config/MASTER.hakios; this #define's HAKIOS_WIFI so the hakios.stubs.c no-ops drop out).
edit(os.path.join(T, "config/MASTER"),
     "options\t\tHAKIOS_ADRENO\t# Adreno 642L GPU/GMU bring-up\t# <hakios_adreno>\n",
     "options\t\tHAKIOS_ADRENO\t# Adreno 642L GPU/GMU bring-up\t# <hakios_adreno>\n"
     "options\t\tHAKIOS_WIFI\t# Wi-Fi of the WCN6750 (wifi=1, wifi_step=N)\t# <hakios_wifi>\n",
     "config/MASTER options HAKIOS_WIFI")

# 9. config/MASTER.arm64.bcm2837: enable the hakios_wifi tag in DEVELOPMENT (via BSD_DEV), the
#    same spot goodix/adreno were added.
edit(os.path.join(T, "config/MASTER.arm64.bcm2837"),
     "#  BSD_DEV =        [ BSD_BASE config_netboot config_imgsrc_access pgo config_vnguard rosetta hakios_goodix hakios_adreno BSD_EXTRAS_DEV ]",
     "#  BSD_DEV =        [ BSD_BASE config_netboot config_imgsrc_access pgo config_vnguard rosetta hakios_goodix hakios_adreno hakios_wifi BSD_EXTRAS_DEV ]",
     "MASTER.arm64.bcm2837 BSD_DEV += hakios_wifi")

print("### WIFI WIRE DONE")
