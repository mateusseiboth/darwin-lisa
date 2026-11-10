#!/usr/bin/env python3
# darwin-lisa: the system never sleeps unless syssleep=1 (iokit/Kernel/IOPMrootDomain.cpp), as HakiOS 16's kernel did
# (research/hackios-xnu-kernel/xnu/hakios.patch, gSleepDisabledFlag = 1).
#
# Why: there is nothing on lisa to sleep into and nothing that would wake it. On 2026-11-10 (evening Boot 1a,
# research/hackios-xnu-kernel/logs/evening-b1a.log) powerd sent kPMMinutesToSleep = 0x7fffffff at 47 s; on iOS that
# means "idle sleep after idleMilliSeconds" (1000 ms), the idle timer expired, PMRD went ON -> SLEEP, and the kernel
# panicked in IOCPUSleepKernel: "Attempting to sleep before all CPUs are registered" (sched_prim.c). The boot-arg
# noidle=1 only keeps the idle timer from being armed; this refuses every system sleep (idle and demand) with
# kPMConfigPreventSystemSleep, the system stays awake with its display off, and a tap or the side button wakes the
# screen.
#
# Edits:
#   - gSleepDisabledFlag starts at 1;
#   - start() reads syssleep=1 (the old behaviour back), next to noidle;
#   - the device tree's sleep-disabled can set the flag but no longer clear it.
# Test: sh kernel/drivers/power/test/run-tests.sh. Idempotent; XNU_WORK=<dir> for a dry run on a copy. Run in docker as
# root (the tree is root's), then the rebuild. Order: independent of every other wire.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402

P = "iokit/Kernel/IOPMrootDomain.cpp"

w.edit(P,
       "static uint32_t         gSleepDisabledFlag = 0;\n",
       "/* darwin-lisa (kernel/drivers/power/nosleep-wire.py): no system sleep on lisa; syssleep=1 gives it back */\n"
       "static uint32_t         gSleepDisabledFlag = 1;\n",
       "IOPMrootDomain.cpp: system sleep refused by default")
w.edit(P,
       "\tPE_parse_boot_argn(\"noidle\", &gNoIdleFlag, sizeof(gNoIdleFlag));\n",
       "\tPE_parse_boot_argn(\"noidle\", &gNoIdleFlag, sizeof(gNoIdleFlag));\n"
       "\t{\n"
       "\t\tuint32_t syssleep = 0;\n"
       "\n"
       "\t\tif (PE_parse_boot_argn(\"syssleep\", &syssleep, sizeof(syssleep)) && syssleep) {\n"
       "\t\t\tgSleepDisabledFlag = 0;\n"
       "\t\t}\n"
       "\t}\n",
       "IOPMrootDomain.cpp: syssleep=1")
w.edit(P,
       "\t\t\tif (PE_get_default(\"sleep-disabled\", &gSleepDisabledFlag, sizeof(gSleepDisabledFlag))) {\n",
       "\t\t\tuint32_t dlisa_dt_sleep_disabled = 0;\n"
       "\n"
       "\t\t\tif (PE_get_default(\"sleep-disabled\", &dlisa_dt_sleep_disabled, sizeof(dlisa_dt_sleep_disabled)) &&\n"
       "\t\t\t    dlisa_dt_sleep_disabled) {\n"
       "\t\t\t\tgSleepDisabledFlag = dlisa_dt_sleep_disabled;\n",
       "IOPMrootDomain.cpp: the device tree cannot clear the default")
print("### NOSLEEP WIRE DONE")
