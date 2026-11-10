#!/usr/bin/env python3
# darwin-lisa DIAGNOSTIC: one kernel line per exec and per process exit, so the
# boot can be followed after logd takes launchd's log off the console:
#   DLISA exec <pid>(<name>)            (xpcproxy's own exec is left out)
#   DLISA exit <pid>(<name>) xstat=...  (wait status: signal in the low 7 bits)
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
EXEC = f"{T}/bsd/kern/kern_exec.c"
EXIT = f"{T}/bsd/kern/kern_exit.c"

EXEC_ANCHOR = "\tbcopy((caddr_t)imgp->ip_ndp->ni_cnd.cn_nameptr, (caddr_t)p->p_comm, buflen);\n\tp->p_comm[buflen] = '\\0';\n"
EXEC_ADD = "\tif (strcmp(p->p_comm, \"xpcproxy\") != 0) {\n\t\tprintf(\"DLISA exec %d(%s)\\n\", proc_getpid(p), p->p_comm);\t/* darwin-lisa diag */\n\t}\n"

EXIT_ANCHOR = "\tuth = current_uthread();\n\n\tproc_lock(p);\n\tproc_transstart(p, 1, 0);\n"
EXIT_ADD = "\tprintf(\"DLISA exit %d(%s) xstat=0x%x\\n\", proc_getpid(p), p->p_comm, p->p_xstat);\t/* darwin-lisa diag */\n"


def edit(path, anchor, add, revert):
    with open(path) as f:
        d = f.read()
    if revert:
        d = d.replace(anchor + add, anchor)
    elif add not in d:
        if d.count(anchor) != 1:
            print("proclife: anchor missing in", path); sys.exit(1)
        d = d.replace(anchor, anchor + add, 1)
    with open(path, "w") as f:
        f.write(d)


revert = "--revert" in sys.argv
edit(EXEC, EXEC_ANCHOR, EXEC_ADD, revert)
edit(EXIT, EXIT_ANCHOR, EXIT_ADD, revert)
print("proclife: reverted" if revert else "proclife: patched")
