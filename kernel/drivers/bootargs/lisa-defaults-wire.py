#!/usr/bin/env python3
# darwin-lisa: lisa's own settings as boot-arg defaults built into the kernel, turned on by the one boot-arg lisa=1.
#
# Why: the standalone (autoboot) line is cut by the loader at 607 characters, its own words take ~119 of them, and the
# line of the evening build would need ~515 more. The phone's hardware constants (touch controller and its supply, the
# processor clock, the panel's geometry, the Wi-Fi interrupt mode and address) are the same at every boot, so they live
# here instead of on the line.
#
# How (pexpert/gen/bootargs.c): PE_parse_boot_argn() and PE_parse_boot_arg_str() look at the line first, as always.
# Only when the name is NOT on the line, and the line has lisa=1, they look it up in DLISA_DEFAULTS below. So:
#   - a line without lisa=1 (every host run: run-flashroot.sh, gpu-run.sh, wifi-run.sh) means exactly what it meant;
#   - an argument on the line always wins (touchscreen=0, fbwidth=1125 fbheight=2499, wifi_irq=1...);
#   - the old explicit arguments are still accepted, with or without lisa=1.
# Only the kernel sees these defaults. What the loader reads (cpus=, bootcpu=, battery=1, ufs=, ufs_data=, rd=,
# loader_return_after=) and what userland reads from kern.bootargs (console_open=1, init_run=, wifi=1, dlisa_*) must
# stay on the line, so none of them is here. TUNABLE()s (fbwidth, fbheight) go through PE_parse_boot_argn() too.
#
# Test: sh kernel/drivers/bootargs/test/run-tests.sh (the pristine bootargs.c wired on a copy, twice, then run on the
# host). Idempotent; XNU_WORK=<dir> for a dry run on a copy. Run in docker as root (the tree is root's), then the
# rebuild. Order: independent of every other wire.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402

DLISA_DEFAULTS = ("touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix touch_goodix_a=1 "
                  "cpufreq=1 cpufreq_max=2400 fbwidth=1080 fbheight=2400 wifi_irq=0 wifi_mac=02:6c:69:73:61:01")

B = "pexpert/gen/bootargs.c"

w.edit(B,
       "boolean_t\nPE_parse_boot_argn(\n",
       "/*\n"
       " * darwin-lisa (kernel/drivers/bootargs/lisa-defaults-wire.py): lisa's hardware settings, for a line with lisa=1\n"
       " * that does not name them. The line always wins; without lisa=1 they are never looked at.\n"
       " */\n"
       f"static char dlisa_defaults[] = \"{DLISA_DEFAULTS}\";\n"
       "\n"
       "static boolean_t\n"
       "dlisa_parse_default(const char *arg_string, void *arg_ptr, int max_len, boolean_t force_string)\n"
       "{\n"
       "\tuint32_t on = 0;\n"
       "\n"
       "\tif (!PE_parse_boot_argn_internal(PE_boot_args(), \"lisa\", &on, sizeof(on), FALSE) || on == 0) {\n"
       "\t\treturn FALSE;\n"
       "\t}\n"
       "\treturn PE_parse_boot_argn_internal(dlisa_defaults, arg_string, arg_ptr, max_len, force_string);\n"
       "}\n"
       "\n"
       "boolean_t\nPE_parse_boot_argn(\n",
       "bootargs.c: lisa's defaults (lisa=1)")
w.edit(B,
       "\treturn PE_parse_boot_argn_internal(PE_boot_args(), arg_string, arg_ptr, max_len, FALSE);\n",
       "\treturn PE_parse_boot_argn_internal(PE_boot_args(), arg_string, arg_ptr, max_len, FALSE) ||\n"
       "\t       dlisa_parse_default(arg_string, arg_ptr, max_len, FALSE);\n",
       "bootargs.c: PE_parse_boot_argn falls back to lisa's defaults")
w.edit(B,
       "\treturn PE_parse_boot_argn_internal(PE_boot_args(), arg_string, arg_ptr, strlen, TRUE);\n",
       "\treturn PE_parse_boot_argn_internal(PE_boot_args(), arg_string, arg_ptr, strlen, TRUE) ||\n"
       "\t       dlisa_parse_default(arg_string, arg_ptr, strlen, TRUE);\n",
       "bootargs.c: PE_parse_boot_arg_str falls back to lisa's defaults")
print("### LISA DEFAULTS WIRE DONE")
