#!/usr/bin/env python3
# darwin-lisa DIAGNOSTIC: trace every BSD syscall Apple's launchd (pid 1, after
# init26 execs /sbin/launchd.darwin) makes: number, error, return value, first
# args, and the path for path-taking calls. Shows exactly where dyld's
# shared-cache load gives up. Idempotent; run inside docker (root). --revert.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/bsd/dev/arm/systemcalls.c"

ANCHOR = "\tAUDIT_SYSCALL_EXIT(code, proc, uthread, error);\n"
ADD = """\t/* darwin-lisa diag: syscall trace for Apple launchd (pid 1) */
\tif (__improbable(pid == 1 && strncmp(proc->p_comm, "launchd.darwin", 14) == 0)) {
\t\tchar dl_path[96];
\t\tsize_t dl_len = 0;
\t\tuser_addr_t dl_pa = 0;
\t\tdl_path[0] = '\\0';
\t\tswitch (code) {
\t\tcase 5: case 33: case 188: case 190: case 338: case 340: case 220: case 154:
\t\t\tdl_pa = (user_addr_t)uthread->uu_arg[0]; break;
\t\tcase 463: case 469: case 472: case 466:
\t\t\tdl_pa = (user_addr_t)uthread->uu_arg[1]; break;
\t\tdefault: break;
\t\t}
\t\tif (dl_pa && copyinstr(dl_pa, dl_path, sizeof(dl_path), &dl_len) != 0) {
\t\t\tdl_path[0] = '?'; dl_path[1] = '\\0';
\t\t}
\t\tprintf("SC1 %u e=%d rv=0x%llx a=0x%llx,0x%llx,0x%llx,0x%llx %s\\n", code, error,
\t\t    (unsigned long long)uthread->uu_rval[0],
\t\t    (unsigned long long)uthread->uu_arg[0], (unsigned long long)uthread->uu_arg[1],
\t\t    (unsigned long long)uthread->uu_arg[2], (unsigned long long)uthread->uu_arg[3], dl_path);
\t}
"""

revert = "--revert" in sys.argv
with open(F) as f:
    d = f.read()
if revert:
    d = d.replace(ANCHOR + ADD, ANCHOR)
    print("sctrace: reverted")
elif ADD in d:
    print("sctrace: already present")
elif ANCHOR in d:
    d = d.replace(ANCHOR, ANCHOR + ADD, 1)
    print("sctrace: added")
else:
    print("sctrace: anchor missing"); sys.exit(1)
with open(F, "w") as f:
    f.write(d)
