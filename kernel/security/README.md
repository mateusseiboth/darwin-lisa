# Caminho B: HakiOS security stand-ins (staged for integration after APFS)

HakiOS lets unsigned iOS userland run without Apple's AMFI/sandbox kexts via minimal stand-ins, compiled
into the kernel. Staged here from xnu-8796 for the port to xnu-12377 (integrate after the APFS + platform
drivers land):
- `miniamfi.c` + `miniamfi_entitlements.cpp` — registers an `amfi_t` via `amfi_interface_register()` at
  `STARTUP(EARLY_BOOT)`, answering entitlement/trust-cache queries. xnu-12377 has `libkern/amfi/amfi.h`, so
  the rebase is mostly matching the `amfi_t` struct fields to the 26 version.
- `kern_minimac.c` — registers minimal MACF policies (miniamfi + minisandbox) via `mac_policy_register()`
  with `.mpo_vnode_check_signature` / `.mpo_policy_syscall` / `.mpo_priv_grant` (stable MACF KPIs in 26).

## AMFI requests answered by the miniamfi policy syscall (kern_minimac.c)

| Request | What | Answer |
| --- | --- | --- |
| 0x5a | dyld policy | everything allowed (DYLD_INSERT_LIBRARIES works, see `userland/inject/`) |
| 0x60 | boot-arg state | "not enforcing" (ExtensionKit) |
| 0x66 | `amfi_launch_constraint_matches_process()` | every process matches (error code AICMR_MATCH = 0). `amfi-lcmatch-wire.py` adds it to this file and to the xnu-work copy. libxpc checks XPC session peer requirements with it and asserts the boolean and the code agree; without the answer backboardd died at every start with `assertion failure: "matched == (match_result.error_code == AICMR_MATCH)" -> 0`. |
