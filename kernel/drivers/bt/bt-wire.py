#!/usr/bin/env python3
# darwin-lisa: Bluetooth of the WCN6750 (QUP0 SE7 UART, polled) as the optional tag `dlisa_bt` (option DLISA_BT):
#   bsd/dev/dlisa.bt.c         bt=1..4; bt=4: /dev/hakios-bt, /dev/uart.bluetooth, kern_ctl com.apple.uart.bluetooth,
#                              kern.hakios.bt_status, kern.hakios.bt_scan
#   bsd/dev/dlisa.spmi.c       the shared SPMI arbiter code (the supplies' state); bsd/sys/dlisa.spmi.h its header
#   bsd/dev/dlisa.periph_fw.c  dlisa_periph_file(): msbtfw11.mbn/.tlv, msnv11.bin (fw/ of the image, else built in:
#                              bsd/dev/dlisa.periph_fw.h from ../periph-fw/periph-fw.py, given with PERIPH_FW_HEADER=)
# The shared files get a second `files` line each for this tag (XNU's config: a file seen invisible first is made
# visible by a later line whose option is on). dlisa_bt_init() is NOT added to hakios.input.c here: that call is made
# by hand. Inert without bt=. Idempotent; XNU_WORK=<dir> for a dry run on a copy. Run in docker as root, then rebuild.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402
import input_chain  # noqa: E402

w.require("bsd/dev/hakios.platform.c", "hakios_spmi_lock(void)", "the shared SPMI lock")
w.require("bsd/dev/hakios.platform.c", "hakios_rsc_lock", "the shared RPMh sequencer lock")
w.copy(os.path.join(HERE, "..", "rtc", "dlisa.spmi.h"), "bsd/sys/dlisa.spmi.h")
w.copy(os.path.join(HERE, "..", "rtc", "dlisa.spmi.c"), "bsd/dev/dlisa.spmi.c")
w.copy(os.path.join(HERE, "..", "periph-fw", "dlisa.periph_fw.c"), "bsd/dev/dlisa.periph_fw.c")
HEADER = os.environ.get("PERIPH_FW_HEADER")
if HEADER:
    w.copy(HEADER, "bsd/dev/dlisa.periph_fw.h")
w.copy(os.path.join(HERE, "dlisa.bt.c"), "bsd/dev/dlisa.bt.c")
w.append_once("bsd/conf/files", "bsd/dev/dlisa.bt.c",
              "\n# darwin-lisa Bluetooth: the WCN6750 over QUP0 SE7's UART, polled (optional; bt=1..4)\n"
              "bsd/dev/dlisa.spmi.c\t\toptional dlisa_bt\n"
              "bsd/dev/dlisa.periph_fw.c\t\toptional dlisa_bt\n"
              "bsd/dev/dlisa.bt.c\t\toptional dlisa_bt\n", "bsd/conf/files")
w.add_option("DLISA_BT", "Bluetooth: WCN6750 over UART (bt=1..4)", "dlisa_bt")
w.add_bsd_dev_tag("dlisa_bt")
input_chain.add_init_call("DLISA_BT", "dlisa_bt_init")
print("### BT WIRE DONE")
