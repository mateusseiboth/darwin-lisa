#!/usr/bin/env python3
# darwin-lisa: what the Wi-Fi join needs on iOS 26, on top of wifi-wire.py + wifi-ath11k-wire.sh (tag hakios_wifi):
#   bsd/dev/hakios.apple80211.c   the vendored file (../hakios.apple80211.c), now answering 26's request shapes too:
#                                 ASSOCIATE 908 bytes, SCAN_REQ 5528, SCAN_RESULT 2272 with TIMESTAMP, STATE as a buffer
#   bsd/dev/hakios.image.c        the vendored file (../hakios.image.c): fw/NAME also from the flash's root
#                                 (ufs_root=1 ufs_part=NAME) when no RAM disk has it, so a boot from the flash alone
#                                 finds the WPSS firmware, the board data and the regulatory db (image_ufs=0: not)
#   bsd/dev/dlisa.wifi_net.c      kern.hakios.wifi_network: init26 gives the kept network, the radio joins it
# Inert at run time without wifi=1 (the image change only answers a firmware request that would have failed).
# Needs hakios_wifi wired (wifi-wire.py). Idempotent; XNU_WORK=<dir> for a dry run on a copy. Run in docker as root,
# then the rebuild (the GPU worker's combined build).
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVERS = os.path.dirname(HERE)
sys.path.insert(0, DRIVERS)
import dlisa_wire as w  # noqa: E402

w.require("bsd/conf/files", "bsd/dev/hakios.sta.c", "hakios_wifi wired first (wifi-wire.py)")
w.require("bsd/dev/hakios.ufs.c", "hakios_ufs_read_partition(const char *name", "the UFS driver's partition reader")
w.copy(os.path.join(DRIVERS, "hakios.apple80211.c"), "bsd/dev/hakios.apple80211.c")
w.copy(os.path.join(DRIVERS, "hakios.image.c"), "bsd/dev/hakios.image.c")
w.copy(os.path.join(HERE, "dlisa.wifi_net.c"), "bsd/dev/dlisa.wifi_net.c")
w.insert_after_line("bsd/conf/files", "bsd/dev/hakios.apple80211.c",
                    "bsd/dev/dlisa.wifi_net.c\t\toptional hakios_wifi\n", "bsd/conf/files += dlisa.wifi_net.c")
print("### WIFI JOIN WIRE DONE")
