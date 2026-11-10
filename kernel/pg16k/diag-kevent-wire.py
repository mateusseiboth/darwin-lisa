#!/usr/bin/env python3
# darwin-lisa DIAGNOSTIC: every kevent registration the kernel refuses (EV_ERROR back to user space), at most
# 300 lines:   DLISA kevent <pid>(<name>) filter <n> ident <id> flags 0x.. fflags 0x.. -> error <e>
# For libdispatch's xpc SIGTERM unote (EVFILT_SIGNAL, SIGTERM) delivering SIGTERM_RECEIVED with no SIGTERM sent:
# libxpc then logs "Bogus check-in attempt" and its peers see XPC_INTERRUPTED_REPLY.
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/bsd/kern/kern_event.c"

ANCHOR = ("out:\n\t/* output local errors through the kevent */\n\tif (error) {\n"
          "\t\tkev->flags |= EV_ERROR;\n\t\tkev->data = error;\n\t}\n\treturn result;\n}\n")
ADD = ("out:\n\t/* output local errors through the kevent */\n\tif (error) {\n"
       "\t\tstatic uint32_t dlisa_kevent_lines;\t/* darwin-lisa diag */\n"
       "\t\tif (dlisa_kevent_lines++ < 300) {\n"
       "\t\t\tprintf(\"DLISA kevent %d(%s) filter %d ident %llu flags 0x%x fflags 0x%x -> error %d\\n\",\n"
       "\t\t\t    proc_getpid(current_proc()), current_proc()->p_comm, kev->filter, (unsigned long long)kev->ident,\n"
       "\t\t\t    kev->flags, kev->fflags, error);\n\t\t}\n"
       "\t\tkev->flags |= EV_ERROR;\n\t\tkev->data = error;\n\t}\n\treturn result;\n}\n")

# what an EVFILT_SIGNAL(SIGTERM) knote delivers to user space (status, flags, data):
#   DLISA sigknote <pid>(<name>) status 0x.. flags 0x.. data <d> fflags 0x..
EV_ANCHOR = ("\tif ((result & FILTER_ACTIVE) == 0) {\n\t\tif ((kn->kn_status & KN_ACTIVE) == 0) {\n")
EV_ADD = ("\tif (kn->kn_filter == EVFILT_SIGNAL && kn->kn_id == SIGTERM && (result & FILTER_ACTIVE)) {\t/* darwin-lisa diag */\n"
          "\t\tprintf(\"DLISA sigknote %d(%s) status 0x%x flags 0x%x data %lld fflags 0x%x\\n\", proc_getpid(current_proc()),\n"
          "\t\t    current_proc()->p_comm, kn->kn_status, kev.flags, (long long)kev.data, kev.fflags);\n\t}\n")

s = open(F).read()
if "--revert" in sys.argv:
    s = s.replace(EV_ADD + EV_ANCHOR, EV_ANCHOR)
elif EV_ADD not in s:
    if s.count(EV_ANCHOR) != 1:
        print("diag-kevent: sigknote anchor found", s.count(EV_ANCHOR), "times")
        sys.exit(1)
    s = s.replace(EV_ANCHOR, EV_ADD + EV_ANCHOR, 1)
    print("diag-kevent: sigknote patched")
if "--revert" in sys.argv:
    s = s.replace(ADD, ANCHOR)
    print("diag-kevent: reverted")
elif ADD in s:
    print("diag-kevent: already applied")
elif s.count(ANCHOR) != 1:
    print("diag-kevent: anchor found", s.count(ANCHOR), "times")
    sys.exit(1)
else:
    s = s.replace(ANCHOR, ADD, 1)
    print("diag-kevent: patched")
open(F, "w").write(s)
