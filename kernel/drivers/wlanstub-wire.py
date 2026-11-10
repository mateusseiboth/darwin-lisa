#!/usr/bin/env python3
# darwin-lisa: boot-arg wlan_stub=1 publishes the Wi-Fi registry objects (HakiosWLANStart(): an IO80211Controller with
# a MAC address, local-mac-address on the device tree's "wlan" node, the en0 interface) without the radio.
#
# Why: MobileGestalt answers lsd's question for the Wi-Fi address of every bundle it registers from those objects;
# without them each question waits up to 5 s for an IONetworkController (hakios.wlan.cpp, found by ios-sapphire), and
# the LaunchServices database of a first boot (empty /private/var, every boot here) takes hours. SpringBoard waits on
# that database ("Database was seeding; client will retry and block") and never gets past the boot progress bar.
# wifi=1 does the same as part of the whole radio bring-up; this is the registry part alone.
# The address is wifi_mac=XX:XX:XX:XX:XX:XX when given, else a fixed locally administered one.
#
# Appends hakios_wlan_stub_init() to iokit/Kernel/hakios.wlan.cpp (tag hakios_wifi) and calls it from bsd_init after
# hakios_keystore_init(); a stub in hakios.stubs.c for a kernel without the tag. Idempotent; run in docker as root after
# wifi-wire.py and keystore/keystore-wire.py, then .work/run-rebuild.sh.
import os
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"

STUB_FN = r'''
/* darwin-lisa: boot-arg wlan_stub=1, the registry objects without the radio (kernel/drivers/wlanstub-wire.py) */
#include <pexpert/pexpert.h>
extern "C" void hakios_wlan_stub_init(void);

extern "C" void
hakios_wlan_stub_init(void)
{
	uint32_t on = 0, wifi = 0;
	uint8_t address[6] = { 0x02, 0x6c, 0x69, 0x73, 0x61, 0x01 };   /* locally administered: 02:"lisa":01 */
	char text[32];

	if (!PE_parse_boot_argn("wlan_stub", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (PE_parse_boot_argn("wifi", &wifi, sizeof(wifi)) && wifi != 0) {
		return;                 /* the radio's own bring-up publishes them */
	}
	if (PE_parse_boot_arg_str("wifi_mac", text, sizeof(text))) {
		for (int i = 0, at = 0; i < 6 && text[at] != 0; i++, at += 3) {
			unsigned value = 0;

			for (int k = 0; k < 2; k++) {
				char c = text[at + k];

				value = value * 16 + (unsigned) (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
			}
			address[i] = (uint8_t) value;
		}
	}
	IOLog("HakiosWLAN: wlan_stub=1: en0 %02x:%02x:%02x:%02x:%02x:%02x without the radio\n", address[0], address[1],
	    address[2], address[3], address[4], address[5]);
	HakiosWLANStart("en0", address);
}
'''


def edit(path, old, new, tag):
    s = open(path).read()
    if new in s:
        print(f"  [skip] {tag}")
        return
    if s.count(old) != 1:
        print(f"  [FAIL] {tag}: anchor found {s.count(old)} times")
        sys.exit(2)
    open(path, "w").write(s.replace(old, new, 1))
    print(f"  [ok]   {tag}")


def append_once(path, marker, text, tag):
    s = open(path).read()
    if marker in s:
        print(f"  [skip] {tag}")
        return
    open(path, "a").write(text)
    print(f"  [ok]   {tag}")


append_once(os.path.join(T, "iokit/Kernel/hakios.wlan.cpp"), "hakios_wlan_stub_init", STUB_FN,
            "hakios.wlan.cpp += hakios_wlan_stub_init")
edit(os.path.join(T, "bsd/kern/bsd_init.c"),
     "\t\t\thakios_keystore_init(); /* darwin-lisa: AppleKeyStore stand-in (boot-arg keystore=1) */\n",
     "\t\t\thakios_keystore_init(); /* darwin-lisa: AppleKeyStore stand-in (boot-arg keystore=1) */\n"
     "\t\t\textern void hakios_wlan_stub_init(void);\n"
     "\t\t\thakios_wlan_stub_init(); /* darwin-lisa: Wi-Fi registry objects without the radio (wlan_stub=1) */\n",
     "bsd_init.c: call hakios_wlan_stub_init")
append_once(os.path.join(T, "bsd/dev/hakios.stubs.c"), "hakios_wlan_stub_init",
            "\n#ifndef HAKIOS_WIFI\nvoid hakios_wlan_stub_init(void);\nvoid hakios_wlan_stub_init(void) {}\n#endif\n",
            "hakios.stubs.c += hakios_wlan_stub_init stub")
print("### WLANSTUB WIRE DONE")
