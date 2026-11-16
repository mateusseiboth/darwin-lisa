#!/usr/bin/env python3
# darwin-lisa: the vibration motor (AW8624 over bit-banged I2C) as the optional tag `dlisa_haptic` (option DLISA_HAPTIC):
#   bsd/dev/dlisa.haptic.c     haptic=1..5; haptic=5: /dev/hakios-haptic + kern.hakios.haptic_play
#   bsd/dev/dlisa.periph_fw.c  dlisa_periph_file(): fw/ of the image, else built in (bsd/dev/dlisa.periph_fw.h, made
#                              here by ../periph-fw/periph-fw.py from this PC's blobs; never in git)
# and dlisa_haptic_init() in hakios_input_init(). Inert without haptic=. Idempotent; XNU_WORK=<dir> for a dry run.
# Run in docker as root (the header needs zstd: run `periph-fw.py header` on the host first if docker lacks it, see
# README), then the rebuild.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402
import input_chain  # noqa: E402

w.copy(os.path.join(HERE, "dlisa.haptic.c"), "bsd/dev/dlisa.haptic.c")
w.copy(os.path.join(HERE, "..", "periph-fw", "dlisa.periph_fw.c"), "bsd/dev/dlisa.periph_fw.c")
HEADER = os.environ.get("PERIPH_FW_HEADER")
if HEADER:
    w.copy(HEADER, "bsd/dev/dlisa.periph_fw.h")
w.append_once("bsd/conf/files", "bsd/dev/dlisa.haptic.c",
              "\n# darwin-lisa vibration motor: AW8624 over I2C by hand (optional; haptic=1..5)\n"
              "bsd/dev/dlisa.periph_fw.c\t\toptional dlisa_haptic\n"
              "bsd/dev/dlisa.haptic.c\t\toptional dlisa_haptic\n", "bsd/conf/files")
w.add_option("DLISA_HAPTIC", "vibration motor: AW8624 (haptic=1..5)", "dlisa_haptic")
w.add_bsd_dev_tag("dlisa_haptic")
input_chain.add_init_call("DLISA_HAPTIC", "dlisa_haptic_init")
print("### HAPTIC WIRE DONE")
