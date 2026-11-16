#!/usr/bin/env python3
# darwin-lisa: the flashlight (PM8350C flash LED over SPMI) as the optional tag `dlisa_torch` (option DLISA_TORCH):
#   bsd/dev/dlisa.torch.c   torch=1 look, torch=2 once, torch=3 /dev/hakios-torch + kern.hakios.torch_level
#   bsd/dev/dlisa.spmi.c    the shared arbiter code (a second files line: visible for any of its tags)
# and dlisa_torch_init() in hakios_input_init(), after the buttons'. Inert without torch=. Idempotent;
# XNU_WORK=<dir> for a dry run on a copy. Run in docker as root, then the rebuild.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402
import input_chain  # noqa: E402

w.require("bsd/dev/hakios.platform.c", "hakios_spmi_lock(void)", "the shared SPMI lock")
w.copy(os.path.join(HERE, "..", "rtc", "dlisa.spmi.h"), "bsd/sys/dlisa.spmi.h")
w.copy(os.path.join(HERE, "..", "rtc", "dlisa.spmi.c"), "bsd/dev/dlisa.spmi.c")
w.copy(os.path.join(HERE, "dlisa.torch.c"), "bsd/dev/dlisa.torch.c")
w.append_once("bsd/conf/files", "bsd/dev/dlisa.torch.c",
              "\n# darwin-lisa flashlight: the PM8350C flash LED over SPMI (optional; torch=1..3)\n"
              "bsd/dev/dlisa.spmi.c\t\toptional dlisa_torch\n"
              "bsd/dev/dlisa.torch.c\t\toptional dlisa_torch\n", "bsd/conf/files")
w.add_option("DLISA_TORCH", "flashlight: PM8350C flash LED (torch=1..3)", "dlisa_torch")
w.add_bsd_dev_tag("dlisa_torch")
input_chain.add_init_call("DLISA_TORCH", "dlisa_torch_init")
print("### TORCH WIRE DONE")
