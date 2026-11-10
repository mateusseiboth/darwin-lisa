#!/usr/bin/env python3
# darwin-lisa DIAGNOSTIC: a user data abort inside the shared-region range
# (0x180000000-0x300000000) that vm_fault() refuses: print the registers that
# libobjc's map_images loop uses (x19 count, x22 mh, x27 &infos[i], x28 i) and the
# _dyld_objc_notify_mapped_info entries around x27 (mh, path), so we can see which
# image dyld handed objc with a bad mach_header.
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/osfmk/arm64/sleh.c"

ANCHOR = """		if (__improbable(result == KERN_FAILURE)) {
			panic("vm_fault() KERN_FAILURE from user fault on thread %p", thread);
		}

		codes[0] = result;
"""
ADD = """		/* darwin-lisa diag: a refused access in the shared region: what objc was handed */
		if (fault_addr >= 0x180000000ULL && fault_addr < 0x300000000ULL) {
			extern char *proc_name_address(void *p);
			void *dl_bsd = get_bsdtask_info(current_task());
			uint64_t dl_x27 = get_saved_state_reg(state, 27);
			printf("DLISA srfault %s far=0x%llx kr=%d pc=0x%llx lr=0x%llx x19=0x%llx x22=0x%llx x27=0x%llx x28=0x%llx\\n",
			    dl_bsd ? proc_name_address(dl_bsd) : "?", (uint64_t)fault_addr, result,
			    get_saved_state_pc(state), get_saved_state_lr(state),
			    get_saved_state_reg(state, 19), get_saved_state_reg(state, 22),
			    dl_x27, get_saved_state_reg(state, 28));
			for (int dl_k = 0; dl_k < 4; dl_k++) {
				uint64_t dl_ent[4];
				char dl_path[120];
				size_t dl_len = 0;
				if (copyin((user_addr_t)(dl_x27 - (uint64_t)dl_k * 32), dl_ent, sizeof(dl_ent)) != 0) {
					break;
				}
				dl_path[0] = '\\0';
				if (dl_ent[1] == 0 || copyinstr((user_addr_t)dl_ent[1], dl_path, sizeof(dl_path), &dl_len) != 0) {
					dl_path[0] = '?'; dl_path[1] = '\\0';
				}
				printf("DLISA srfault   info[-%d] mh=0x%llx sect=0x%llx flags=0x%llx %s\\n",
				    dl_k, dl_ent[0], dl_ent[2], dl_ent[3], dl_path);
			}
		}

"""

with open(F) as f:
    d = f.read()
if "--revert" in sys.argv:
    d = d.replace(ADD + "\t\tcodes[0] = result;\n", "\t\tcodes[0] = result;\n")
    print("srfault: reverted")
elif "DLISA srfault" in d:
    print("srfault: already present")
elif d.count(ANCHOR) == 1:
    d = d.replace(ANCHOR, ANCHOR.replace("\t\tcodes[0] = result;\n", ADD + "\t\tcodes[0] = result;\n"), 1)
    print("srfault: patched")
else:
    print("srfault: anchor missing"); sys.exit(1)
with open(F, "w") as f:
    f.write(d)
