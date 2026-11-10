#!/usr/bin/env python3
# darwin-lisa DIAGNOSTIC: print every Mach exception that becomes a UNIX signal
# for a user process: pid, name, exception, code, subcode (fault address for
# EXC_BAD_ACCESS) and the user pc. launchd only reports "exited due to SIGBUS ...
# crashed into the shared cache area"; this says where and why.
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/bsd/uxkern/ux_exception.c"

ANCHOR = "\t/* Translate exception and code to signal type */\n\tint ux_signal = ux_exception(exception, code, subcode);\n"
ADD = """\t/* darwin-lisa diag: where a user process faulted */
\t{
\t\textern user_addr_t get_useraddr(void);
\t\tprintf("DLISA uxexc %d(%s) exc=%d code=0x%llx subcode=0x%llx pc=0x%llx sig=%d\\n",
\t\t    proc_getpid(p), p->p_comm, exception, (unsigned long long)code,
\t\t    (unsigned long long)subcode,
\t\t    (unsigned long long)(thread == current_thread() ? get_useraddr() : 0),
\t\t    ux_signal);
\t}
"""

SR = f"{T}/osfmk/vm/vm_shared_region.c"
SR_ANCHOR = "\t\tif (shared_region->sr_slide == 0 && srfmp->slide != 0) {\n\t\t\tshared_region->sr_slide = srfmp->slide;\n"
SR_ADD = "\t\t\tprintf(\"DLISA shared region slide 0x%x\\n\", (unsigned)srfmp->slide);\n"
with open(SR) as f:
    sr = f.read()
if "--revert" in sys.argv:
    sr = sr.replace(SR_ANCHOR + SR_ADD, SR_ANCHOR)
elif SR_ADD not in sr:
    if SR_ANCHOR not in sr:
        print("uxexc: slide anchor missing"); sys.exit(1)
    sr = sr.replace(SR_ANCHOR, SR_ANCHOR + SR_ADD, 1)
with open(SR, "w") as f:
    f.write(sr)

with open(F) as f:
    d = f.read()
if "--revert" in sys.argv:
    d = d.replace(ANCHOR + ADD, ANCHOR)
    print("uxexc: reverted")
elif ADD in d:
    print("uxexc: already present")
elif ANCHOR in d:
    d = d.replace(ANCHOR, ANCHOR + ADD, 1)
    print("uxexc: added")
else:
    print("uxexc: anchor missing"); sys.exit(1)
with open(F, "w") as f:
    f.write(d)
