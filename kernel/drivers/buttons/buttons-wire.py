#!/usr/bin/env python3
# darwin-lisa: the side and volume keys as the optional tag `hakios_buttons` (option HAKIOS_BUTTONS). Files:
#   bsd/dev/dlisa.buttons.c   the keys through the SPMI arbiter (reads only), button records of /dev/hakios-touch
#   bsd/dev/dlisa.spmi.c      the shared arbiter code (also the RTC's): a second files line makes it visible for
#                             either tag (XNU's config: a file seen invisible first may be made visible by a later line)
# hakios.input.c already calls hakios_buttons_init(); hakios.stubs.c's no-op goes away with HAKIOS_BUTTONS defined.
# On with touchscreen= (lisa=1 gives it) unless buttons=0. Idempotent; XNU_WORK=<dir> for a dry run on a copy. Run in
# docker as root, then the rebuild.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402

w.require("bsd/dev/hakios.platform.c", "hakios_spmi_lock(void)", "the shared SPMI lock")
w.require("bsd/dev/hakios.stubs.c", "#ifndef HAKIOS_BUTTONS", "the buttons' stub guard")
w.require("bsd/dev/hakios.input.c", "hakios_buttons_init();", "the input chain's call")
w.copy(os.path.join(HERE, "..", "rtc", "dlisa.spmi.h"), "bsd/sys/dlisa.spmi.h")
w.copy(os.path.join(HERE, "..", "rtc", "dlisa.spmi.c"), "bsd/dev/dlisa.spmi.c")
w.copy(os.path.join(HERE, "dlisa.buttons.c"), "bsd/dev/dlisa.buttons.c")
w.append_once("bsd/conf/files", "bsd/dev/dlisa.buttons.c",
              "\n# darwin-lisa keys: power, volume up/down through the SPMI arbiter (optional; buttons=1 or touchscreen=1)\n"
              "bsd/dev/dlisa.spmi.c\t\toptional hakios_buttons\n"
              "bsd/dev/dlisa.buttons.c\t\toptional hakios_buttons\n", "bsd/conf/files")
w.add_option("HAKIOS_BUTTONS", "power and volume keys (buttons=1)", "hakios_buttons")
w.add_bsd_dev_tag("hakios_buttons")
print("### BUTTONS WIRE DONE")
