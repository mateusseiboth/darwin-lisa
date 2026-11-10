#!/usr/bin/env python3
# darwin-lisa: the real-time clock as the optional tag `hakios_rtc` (option HAKIOS_RTC). Files:
#   bsd/sys/dlisa.spmi.h   a PMIC peripheral through the SPMI arbiter v5 (shared; the battery may move onto it)
#   bsd/dev/dlisa.spmi.c
#   bsd/dev/dlisa.rtc.c    the PMK8350's clock: dlisa_rtc_get/_set, sysctls kern.hakios.rtc_*
# and two hooks in iokit/Kernel/IOPlatformExpert.cpp, under #ifdef HAKIOS_RTC:
#   PEGetUTCTimeOfDay   when no platform expert answered (none does here), dlisa_rtc_get()
#   PESetUTCTimeOfDay   dlisa_rtc_set(): every settimeofday, so the offset follows the system's time
# Inert unless boot-arg rtc=1 (both functions answer nothing then, as before). Needs hakios.platform.c's
# hakios_spmi_lock (standard). Idempotent; XNU_WORK=<dir> for a dry run on a copy. Run in docker as root, then the
# rebuild (the GPU worker's combined build).
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402

w.require("bsd/dev/hakios.platform.c", "hakios_spmi_lock(void)", "the shared SPMI lock")
w.copy(os.path.join(HERE, "dlisa.spmi.h"), "bsd/sys/dlisa.spmi.h")
w.copy(os.path.join(HERE, "dlisa.spmi.c"), "bsd/dev/dlisa.spmi.c")
w.copy(os.path.join(HERE, "dlisa.rtc.c"), "bsd/dev/dlisa.rtc.c")
w.append_once("bsd/conf/files", "bsd/dev/dlisa.rtc.c",
              "\n# darwin-lisa real-time clock: the PMK8350's RTC over SPMI (optional; rtc=1)\n"
              "bsd/dev/dlisa.spmi.c\t\toptional hakios_rtc\n"
              "bsd/dev/dlisa.rtc.c\t\toptional hakios_rtc\n", "bsd/conf/files")
w.add_option("HAKIOS_RTC", "real-time clock: the PMK8350's RTC (rtc=1)", "hakios_rtc")
w.add_bsd_dev_tag("hakios_rtc")

PE = "iokit/Kernel/IOPlatformExpert.cpp"
w.edit(PE,
       "void\nPEGetUTCTimeOfDay(clock_sec_t * secs, clock_usec_t * usecs)\n{",
       "#ifdef HAKIOS_RTC\n"
       "/* darwin-lisa (kernel/drivers/rtc): the PMK8350's clock, when no platform expert has one (rtc=1) */\n"
       "extern \"C\" bool dlisa_rtc_get(clock_sec_t *secs, clock_nsec_t *nsecs);\n"
       "extern \"C\" void dlisa_rtc_set(clock_sec_t secs);\n"
       "#endif\n\n"
       "void\nPEGetUTCTimeOfDay(clock_sec_t * secs, clock_usec_t * usecs)\n{",
       "IOPlatformExpert.cpp: dlisa_rtc_* declared")
w.edit(PE,
       "\t\tgIOPlatform->getUTCTimeOfDay(secs, &nsecs);\n\t}\n",
       "\t\tgIOPlatform->getUTCTimeOfDay(secs, &nsecs);\n\t}\n"
       "#ifdef HAKIOS_RTC\n"
       "\tif (*secs == 0 && !dlisa_rtc_get(secs, &nsecs)) {\n"
       "\t\tnsecs = 0;\n"
       "\t}\n"
       "#endif\n",
       "IOPlatformExpert.cpp: PEGetUTCTimeOfDay asks the RTC")
w.edit(PE,
       "\t\tgIOPlatform->setUTCTimeOfDay(secs, usecs * NSEC_PER_USEC);\n\t}\n",
       "\t\tgIOPlatform->setUTCTimeOfDay(secs, usecs * NSEC_PER_USEC);\n\t}\n"
       "#ifdef HAKIOS_RTC\n"
       "\tdlisa_rtc_set(secs);\n"
       "#endif\n",
       "IOPlatformExpert.cpp: PESetUTCTimeOfDay tells the RTC")
print("### RTC WIRE DONE")
