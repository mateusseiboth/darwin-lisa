#!/usr/bin/env python3
# darwin-lisa: resolve the one undefined symbol _thread_runnable_and_run_self (HakiOS osfmk
# instrumentation) with a file-local zero no-op in apfs_vnops.c.
import sys
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
p = T + "/bsd/miscfs/apfs/apfs_vnops.c"
old = "extern void thread_runnable_and_run_self(uint64_t *runnable, uint64_t *run);"
new = (
    "/*\n"
    " * darwin-lisa: HakiOS defined thread_runnable_and_run_self() in osfmk/kern/thread.c to split\n"
    " * the APFS lock-wait time (below) into blocked / queued / spinning. It reads per-thread\n"
    " * time-accounting internals that changed between xnu-8796 and xnu-12377 (runnable_timer /\n"
    " * recount). The split is diagnostic only and has no effect on mount or read, so it is a zero\n"
    " * no-op here; porting the real per-thread accounting to xnu-12377 is deferred.\n"
    " */\n"
    "static inline void\n"
    "thread_runnable_and_run_self(uint64_t *runnable, uint64_t *run)\n"
    "{\n"
    "\t*runnable = 0;\n"
    "\t*run = 0;\n"
    "}"
)
s = open(p).read()
if new in s:
    print("[skip] already applied")
    sys.exit(0)
if s.count(old) != 1:
    print(f"[FAIL] anchor count={s.count(old)}")
    sys.exit(2)
open(p, "w").write(s.replace(old, new))
print("[ok] apfs_vnops.c thread_runnable_and_run_self no-op")
