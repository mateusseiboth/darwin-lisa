#!/usr/bin/env python3
# darwin-lisa DIAGNOSTIC: one kernel line per kill(2) of a single process with a terminating signal other than
# SIGKILL's usual traffic, so a daemon that keeps being told to stop can be traced to who tells it:
#   DLISA kill <pid>(<name>) -> <pid>(<name>) sig <n>
# Found containermanagerd taking SIGTERM (libxpc: "Bogus check-in attempt", its clients XPC_INTERRUPTED_REPLY).
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/bsd/kern/kern_sig.c"

ANCHOR = "\t\tif (uap->signum) {\n\t\t\tpsignal(p, uap->signum);\n\t\t}\n\t\tproc_rele(p);\n\t\treturn 0;\n"
ADD = ("\t\tif (uap->signum == SIGTERM || uap->signum == SIGKILL || uap->signum == SIGHUP || uap->signum == SIGINT) {\n"
       "\t\t\tprintf(\"DLISA kill %d(%s) -> %d(%s) sig %d\\n\", proc_getpid(cp), cp->p_comm, proc_getpid(p), p->p_comm,\n"
       "\t\t\t    uap->signum);\t/* darwin-lisa diag */\n\t\t}\n")

# every SIGTERM, from whichever path (launchd does not always use kill(2)):
#   DLISA sigterm -> <pid>(<name>) from <pid>(<name>)
SIG_ANCHOR = "\tmask = sigmask(signum);\n\tprop = sigprop[signum];\n"
SIG_ADD = ("\tif (signum == SIGTERM && p != PROC_NULL) {\n"
           "\t\tprintf(\"DLISA sigterm -> %d(%s) from %d(%s)\\n\", proc_getpid(p), p->p_comm, proc_getpid(current_proc()),\n"
           "\t\t    current_proc()->p_comm);\t/* darwin-lisa diag */\n\t}\n")

# where EVFILT_SIGNAL is told (every path), and an EVFILT_SIGNAL(SIGTERM) knote firing on a hint that is no signal:
#   DLISA knote-sigterm -> <pid>(<name>)      DLISA sigfilt <ident> fired on hint 0x...
KN_ANCHOR = "\t\tproc_knote(sig_proc, NOTE_SIGNAL | signum);\n"
KN_ADD = ("\t\tif (signum == SIGTERM) {\n"
          "\t\t\tprintf(\"DLISA knote-sigterm -> %d(%s)\\n\", proc_getpid(sig_proc), sig_proc->p_comm);\t/* darwin-lisa diag */\n"
          "\t\t}\n")
FILT_ANCHOR = "\t} else if (hint & NOTE_EXIT) {\n\t\tpanic(\"filt_signal: detected NOTE_EXIT event\");\n\t}\n"
FILT_ADD = ("\tif (kn->kn_hook32 != 0 && !(hint & NOTE_SIGNAL)) {\n"
            "\t\tprintf(\"DLISA sigfilt %llu fired on hint 0x%lx\\n\", (unsigned long long)kn->kn_id, hint);\t/* darwin-lisa diag */\n"
            "\t}\n")

s = open(F).read()
for anchor, add, tag in ((ANCHOR, ADD, "kill"), (SIG_ANCHOR, SIG_ADD, "sigterm"), (KN_ANCHOR, KN_ADD, "knote"),
                         (FILT_ANCHOR, None, "filter")):
    if add is None:
        # after the anchor, not before it
        if "--revert" in sys.argv:
            s = s.replace(anchor + FILT_ADD, anchor)
        elif FILT_ADD not in s:
            if s.count(anchor) != 1:
                print("diag-kill: filter anchor missing")
                sys.exit(1)
            s = s.replace(anchor, anchor + FILT_ADD, 1)
        print(f"diag-kill: {tag} done")
        continue
    if "--revert" in sys.argv:
        s = s.replace(add + anchor, anchor)
        print(f"diag-kill: {tag} reverted")
    elif add in s:
        print(f"diag-kill: {tag} already applied")
    elif s.count(anchor) != 1:
        print(f"diag-kill: {tag} anchor missing")
        sys.exit(1)
    else:
        s = s.replace(anchor, add + anchor, 1)
        print(f"diag-kill: {tag} patched")
open(F, "w").write(s)
