#!/bin/sh
# Add the plain-arm64 kernel link flags the prelinker needs (split-seg + chained
# fixups) to a fetched UniXNU xnu-work tree's makedefs/MakeInc.def. Idempotent.
# Run inside the docker toolchain (the work tree is root-owned). Then rebuild with
# XNU_KERNEL_CHAINED_FIXUPS=1 (delete mach.*.unstripped* first to force a relink).
# See README.md and ../../docs/phase3-ondevice.md.
set -eu
MK=${1:?usage: apply-build-flags.sh <xnu-work>/makedefs/MakeInc.def}
python3 -I - "$MK" <<'PY'
import sys
p=sys.argv[1]
s=open(p).read()
anchor="\t-Wl,-add_split_seg_info\n\nLDFLAGS_KERNEL_ONLY_SUBARCH_x86_64 ="
block=("LDFLAGS_KERNEL_ONLY_SUBARCH_arm64 = \\\n"
       "\t-Wl,-add_split_seg_info \\\n"
       "\t-Wl,-fixup_chains \\\n"
       "\t-Wl,-kernel\n\n")
if "LDFLAGS_KERNEL_ONLY_SUBARCH_arm64 =" in s:
    print("arm64 ldflags already present")
else:
    # insert the arm64 block right before the x86_64 one
    mark="LDFLAGS_KERNEL_ONLY_SUBARCH_x86_64 ="
    i=s.index(mark)
    open(p,'w').write(s[:i]+block+s[i:])
    print("added LDFLAGS_KERNEL_ONLY_SUBARCH_arm64 (split-seg + chained fixups)")
PY
