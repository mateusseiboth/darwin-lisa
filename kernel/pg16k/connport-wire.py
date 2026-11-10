#!/usr/bin/env python3
# darwin-lisa FIX: mach_port_is_connection_for_service() without a message filter (no Sandbox kext).
#
# Every XPC listener on iOS 26 checks each new client: libxpc asks the kernel whether the client's connection port
# was derived from the listener's service port (mach_port_is_connection_for_service). xnu answers KERN_NOT_SUPPORTED
# (46) unless a mach message filter is registered, which only Apple's Sandbox kext does; this port runs minisandbox.
# libxpc then logged "Failed to verify check-in connection port was derived from the listener connection port
# (kr=46)" + "Bogus check-in attempt. Ignoring." and dropped the client: every first message to every launchd
# service came back XPC_INTERRUPTED_REPLY (lsd -> containermanagerd, runningboardd -> cfprefsd, ...). lsd aborted,
# and runningboardd, backboardd and SpringBoard waited on it.
#
# Without a filter there is no policy to look up, which is the same answer xnu gives for a service port that has
# no sandbox label: filter policy 0, KERN_SUCCESS. The port checks before it (valid names, receive right, service
# port type) still run. Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/osfmk/ipc/mach_port.c"

OLD = ("\tif (!mach_msg_filter_at_least(MACH_MSG_FILTER_CALLBACKS_VERSION_1)) {\n"
       "\t\treturn KERN_NOT_SUPPORTED;\n\t}\n\n"
       "\tret = ipc_port_translate_receive(space, service_port_name, &service_port);\n")
NEW = ("\t/* darwin-lisa: no message filter (no Sandbox kext) means no policy, as for an unlabeled service port */\n"
       "\tbool dlisa_no_filter = !mach_msg_filter_at_least(MACH_MSG_FILTER_CALLBACKS_VERSION_1);\n\n"
       "\tret = ipc_port_translate_receive(space, service_port_name, &service_port);\n")
OLD2 = ("\t/* Port is locked and active */\n"
        "\tservice_port_sblabel = label.iol_service->ispl_sblabel;\n")
NEW2 = ("\tif (dlisa_no_filter) {\n"
        "\t\tip_mq_unlock_label_put(service_port, &label);\n"
        "\t\t*filter_policy_id = 0;\n"
        "\t\treturn KERN_SUCCESS;\n\t}\n\n"
        "\t/* Port is locked and active */\n"
        "\tservice_port_sblabel = label.iol_service->ispl_sblabel;\n")

s = open(F).read()
if "--revert" in sys.argv:
    s = s.replace(NEW2, OLD2).replace(NEW, OLD)
    print("connport: reverted")
elif NEW in s:
    print("connport: already applied")
else:
    for old in (OLD, OLD2):
        if s.count(old) != 1:
            print("connport: anchor found", s.count(old), "times:", repr(old[:50]))
            sys.exit(1)
    s = s.replace(OLD, NEW, 1).replace(OLD2, NEW2, 1)
    print("connport: patched")
open(F, "w").write(s)
