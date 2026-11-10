#!/usr/bin/env python3
# darwin-lisa: miniamfi answers AMFI request 0x66, "does this process match this launch constraint"
# (amfi_launch_constraint_matches_process() in libamfi, linked into libxpc).
#
# libxpc checks the peer requirement of an XPC session that way (xpc_peer_requirement_match_received_message) and
# asserts that the boolean and the result's error code agree. With no handler the call failed with ENOPOLICY, the
# caller's zeroed result said "match" while the function said "no match", and backboardd died at every start:
#   assertion failure: "matched == (match_result.error_code == AICMR_MATCH)" -> 0
# Nothing is enforced by this kernel, so every process matches: error code 0 (AICMR_MATCH) in the result.
# Argument: { audit_token_t token; user_addr_t requirement; uint32_t length; user_addr_t result; uint32_t size }.
#
# Edits darwin-lisa/kernel/security/kern_minimac.c (the source) and the xnu-work copy, both idempotent.
# Run inside docker (root), then .work/run-rebuild.sh.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
FILES = (os.path.join(HERE, "kern_minimac.c"), f"{T}/bsd/kern/kern_minimac.c")

DEFINE_ANCHOR = "#define AMFI_BOOTARG_STATE_NOT_ENFORCING 0x86ULL\n"
DEFINE_ADD = """
/*
 * Request 0x66, "does this process match this launch constraint" (amfi_launch_constraint_matches_process()). libxpc
 * asks it for the peer requirement of an XPC session and asserts that the answer's error code says the same as the
 * call: AICMR_MATCH (0) for a match. Nothing is enforced here, so every process matches.
 */
#define AMFI_SYSCALL_LAUNCH_CONSTRAINT_MATCH 0x66
struct amfi_lc_match_args {
	uint8_t     token[32];          /* audit_token_t of the process */
	user_addr_t requirement;
	uint32_t    length;
	user_addr_t result;             /* { uint32_t error_code; char message[1024]; } */
	uint32_t    size;
};
"""
CALL_ANCHOR = "\tif (call == AMFI_SYSCALL_QUERY_BOOTARG_STATE) {\n"
CALL_ADD = """\tif (call == AMFI_SYSCALL_LAUNCH_CONSTRAINT_MATCH) {
		struct amfi_lc_match_args lc;
		uint32_t match = 0;     /* AICMR_MATCH */

		error = copyin(arg, &lc, sizeof(lc));
		if (error == 0 && lc.result != USER_ADDR_NULL && lc.size >= sizeof(match)) {
			error = copyout(&match, lc.result, sizeof(match));
		}
		return error;
	}
"""

for path in FILES:
    s = open(path).read()
    if "--revert" in sys.argv:
        s = s.replace(DEFINE_ANCHOR + DEFINE_ADD, DEFINE_ANCHOR).replace(CALL_ADD + CALL_ANCHOR, CALL_ANCHOR)
        print("amfi-lcmatch: reverted", path)
    elif "AMFI_SYSCALL_LAUNCH_CONSTRAINT_MATCH" in s:
        print("amfi-lcmatch: already in", path)
        continue
    else:
        if s.count(DEFINE_ANCHOR) != 1 or s.count(CALL_ANCHOR) != 1:
            print("amfi-lcmatch: anchors missing in", path)
            sys.exit(1)
        s = s.replace(DEFINE_ANCHOR, DEFINE_ANCHOR + DEFINE_ADD, 1).replace(CALL_ANCHOR, CALL_ADD + CALL_ANCHOR, 1)
        print("amfi-lcmatch: patched", path)
    open(path, "w").write(s)
