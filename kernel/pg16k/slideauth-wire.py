#!/usr/bin/env python3
# darwin-lisa: the kernel is built WITHOUT ptrauth_calls (plain arm64, no PAC on
# this CPU), and vm_shared_region_slide_page_v3/_v5 only write an authenticated
# rebase back inside `#if __has_feature(ptrauth_calls)`. So on this kernel every
# auth pointer in the arm64e dyld shared cache's DATA pages kept its raw chained
# encoding (bit 63, diversity, next) instead of becoming the slid address. Add the
# missing `#else` that stores the plain (unsigned) slid value, which is exactly
# what an unsigned pointer is on a no-PAC CPU.
# Idempotent; run inside docker (root). --revert supported.
import re, sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/osfmk/vm/vm_shared_region.c"
MARK = "#else /* darwin-lisa: no ptrauth, store the unsigned slid pointer */\n"
PAT = re.compile(r"(\} else \{\n(\t+)memcpy\(rebaseLocation, &value, sizeof\(value\)\);\n\t+\}\n)"
                 r"(#endif /\* __has_feature\(ptrauth_calls\) \*/\n)")

with open(F) as f:
    d = f.read()
if "--revert" in sys.argv:
    d = re.sub(re.escape(MARK) + r"\t+memcpy\(rebaseLocation, &value, sizeof\(value\)\);\n", "", d)
    print("slideauth: reverted")
elif MARK in d:
    print("slideauth: already present (%d sites)" % d.count(MARK))
else:
    def add(m):
        ind = m.group(2)[:-1]
        return m.group(1) + MARK + ind + "memcpy(rebaseLocation, &value, sizeof(value));\n" + m.group(3)
    d, n = PAT.subn(add, d)
    print("slideauth: patched %d sites" % n)
    if n < 2:
        print("slideauth: expected >= 2 (v3, v5)"); sys.exit(1)
with open(F, "w") as f:
    f.write(d)
