# Wi-Fi join on iOS 26 (lisa)

The kernel already brings the WCN6750 up and scans with it (`../README-wifi.md`: WPSS, ath11k from Linux 7.0.14, 12
networks heard). This folder adds what a join needs on iOS 26: credentials that never touch the repository, firmware
for a boot from the flash alone, the request shapes iOS 26's wifid sends, and wifid itself in the root.

## Where the 4-way handshake happens

In the kernel. HakiOS's station layer, `wifi/lx/src/lx_80211.c` + `lx_wpa.c` (linked inside `ath11k.o` by
`mk-ath11k.sh`), stands in for mac80211 and wpa_supplicant:

- WPA2-PSK: PBKDF2, the PTK, the 4-way and group-key handshakes.
- HakiOS's review (`research/hackios-xnu-kernel/docs/driver-review/wifi-stack.md`) found the arithmetic right and
  fixed W1, W2, W7, W8, W10 and W12.

No userland supplicant is needed. WPA3-SAE is not implemented: it needs SAE (Dragonfly) in `lx_wpa.c`, about a week
of work. Use a WPA2 or WPA2/WPA3-transition network for the test.

## Two ways to join

**A. The kernel joins by itself.** This path is first for the test: it needs no wifid.

1. With `wifi=1`, `hakios.wifi.c` `join_given()` joins the network in `fw/network` of a RAM disk, as a standing
   request.
2. With a boot from the flash alone, init26 writes the copy kept on the data volume to `kern.hakios.wifi_network`
   (`dlisa.wifi_net.c`). A thread waits for the radio and asks for the same standing join.
3. `en0` goes link-up (`KEV_DL_LINK_ON`).
4. configd's IPConfiguration runs DHCP once an `en0` network service exists. On iOS 26 wifid creates that service
   (`SCNetworkSetEstablishDefaultInterfaceConfiguration`), so wifid needs to run for an IP.

**B. wifid / Settings.** `IO80211.framework` of 26 (read out of the 23F84 dyld cache):

1. It looks for `IOInterfaceName=en0`, which `HakiosWLAN` publishes with `IO80211InterfaceRole=Infrastructure`.
2. It tries the IOService's user client. HakiosWLAN refuses it, so the framework falls back to the old ioctls
   (`SIOCGA80211`/`SIOCSA80211`) once three checks pass:
   - `SIOCGIFTYPE` says ether/Wi-Fi;
   - the interface's role is Infrastructure;
   - CARD_CAPABILITIES answers.
3. It reads events from the PF_SYSTEM kev socket (class 6, subclass 1), as on 16.5.

Four request shapes changed, and `../hakios.apple80211.c` now answers both 16.5's and 26's. Each request is told
apart by its length:

| Request | iOS 26 | Was (16.5) |
| --- | --- | --- |
| ASSOCIATE (20) | 908 bytes, auth fields are u32, ssid_len at 0x10, key at 0x4c | 592 bytes, key at 0x48 |
| SCAN_REQ (10) | 5528 bytes (+ UUID, SCAN_MIN_TIMESTAMP) | 5456: the old code refused anything else (EINVAL) |
| SCAN_RESULT (11), CURRENT_NETWORK (103) | 2272 bytes, 2048 for elements, TIMESTAMP u64 at 0x8d0 | 1164 |
| STATE (13) | `{version, state}` in the data buffer | in req_val only (26 saw INIT for ever) |

Evidence: the subagent's per-function disassembly. It is not kept in the repository; regenerate it with
`tools/ipsw-dl/ipsw dyld disass` on `_performAssociation`, `_getScanData`, `Apple80211GetWithIOCTL` and
`_addScanResultToList`.

wifid on 26 is still `/usr/sbin/wifid` (`com.apple.wifid.plist`). Two things kept it out of the root:

- the LEAN list left it out;
- its `_LimitLoadToDeviceTree = [wlan, marconi-wifi, centauri-alpha, ...]`, and the loader's device tree has no
  such node.

The userland wire fixes both, and init26 disables wifid for every boot without `wifi=1` (or with `wifid=0`), so
other boots behave as before.

## Credentials: never in a file of the repository

- **Host run:** run `tools/wifi-run.sh`.
  1. It asks for the name and passphrase (`read -s`).
  2. It builds a 32 MiB APFS image in `/dev/shm` holding `fw/network` and the Wi-Fi firmware.
  3. `run-flashroot.sh` uploads that image as the resident md0, and the image is deleted when the script exits.
  4. The kernel joins from md0. init26 mounts that RAM disk read-only and copies `fw/network` to
     `/private/var/db/dlisa-wifi/network` (0600, on the HFSX data volume).
- **Later standalone boots** (no PC): init26 gives the kept file to the kernel, and the radio joins it.
- **Forget it:** boot once with `wifi_forget=1`.

Every log line prints lengths only, never a name or a passphrase.

## Firmware on a flash boot

`../hakios.image.c`: when no RAM disk has `fw/NAME` and the root is the flash's (`ufs_root=1 ufs_part=super`), the
file is read from the root's APFS container through `hakios_ufs_read_partition`. The root image already carries
`fw/` (`wpss.mdt`, `wpss.b02..b07`, `board-2.bin`, `regdb_xiaomi.bin`). `*disk_out` is 2 for these files, so
adreno's log line will read `/dev/md2`. The GPU firmware gains the same fallback. Boot-arg `image_ufs=0` turns it
off.

## Files

| File | What |
| --- | --- |
| `dlisa.wifi_net.c` | `kern.hakios.wifi_network` (write-only, masked, root only): parse, wait for the station, standing join |
| `wifi-join-wire.py` | copies `../hakios.apple80211.c`, `../hakios.image.c` and `dlisa.wifi_net.c`; adds the files entry (tag `hakios_wifi`) |
| `../hakios.apple80211.c` | the vendored file, with the 26 shapes (above) |
| `../hakios.image.c` | the vendored file, with the flash-root fallback |
| `../../../userland/init/wifi-net.c` (+ `test/wifi-net-test.c`) | pure: validate the network text; gate wifid on `wifi=1` |
| `../../../userland/init/rtc-wifi-boot.c` | init26: seed from a RAM disk, keep it, give it to the kernel |
| `../../../userland/scripts/rtc-wifi-userland-wire.py` | the userland install step (init-26.c, lean-daemons.txt, plist-tweaks.py) |
| `../../../tools/wifi-run.sh` | the host run with credentials asked for at the moment |

## Verified offline (2026-11-10)

- `wifi-join-wire.py` and `rtc-wifi-userland-wire.py` were each run twice on copies: `[ok]`, then `[skip]`.
- Syntax-only compiles with the tree's flags (`tools/ksyntax.py`): `hakios.apple80211.c`, `hakios.image.c` and
  `dlisa.wifi_net.c` have 0 errors. Their warnings are only `-Wunsafe-buffer-usage`, the same as the tree's other
  files.
- `_Static_assert`s pin the 26 offsets (908 bytes with the key at 0x4c; 2272 bytes with the timestamp at 0x8d0).
- init26 host tests pass (`wifi-net-test`), and the patched init26 links. `test_plist_tweaks.py` passes, and the
  tweak removes `_LimitLoadToDeviceTree`.
- A dummy `fw/network` image made by `mkapfs.py` reads back through the kernel's APFS code (`apfs-tool cat`).
- Not verified (it needs the phone): the association itself, the 26 request shapes in use, DHCP through configd,
  and the UFS read of `fw/` during early boot.

## Integration (combined build)

1. Kernel (docker as root, after `wifi-wire.py` and `wifi-ath11k-wire.sh`, which are already in the tree):
   `python3 darwin-lisa/kernel/drivers/wifi-join/wifi-join-wire.py`, then the single rebuild.
   - `rebuild26.sh` already links `ath11k.o`.
   - **If** `wifi-wire.py` is ever re-run, run this script after it: it copies the same vendored files, so the
     result is the same either way.
2. Userland: `python3 darwin-lisa/userland/scripts/rtc-wifi-userland-wire.py`, then rebuild init26 and the root
   (`mkimage-trim.sh`; LEAN now keeps `com.apple.wifid.plist`, and plist-tweaks drops its device-tree limit).
3. Boot-args for the Wi-Fi run: `wifi=1 wifi_irq=0 wifi_mac=02:6c:69:73:61:01`, plus `wifi_loud=1` for the first
   run. `wlan_stub=1` steps aside by itself.
   - Mind the loader's 420-character limit on the standalone line.
   - Without `wifi=1`, nothing changes: wifid stays disabled.

## Device test

1. `tools/wifi-run.sh` (host run from the flash, data volume on), typing the network at the prompt.

   Log markers:
   - `wifi: THE RADIO IS UP`
   - `wifi: THE NETWORK OF THE FILE IS JOINED`
   - `init26: WIFI the network of /dev/md0's fw/network is kept on the data volume`
   - `apple80211: wifid gets ...` lines, ending with the per-request summary
   - configd's DHCP
2. A standalone cold boot with `wifi=1` baked in, and no PC.

   Log markers:
   - `hakios_image` / `wifi:` firmware read from the flash (no "no fw/" lines)
   - `init26: WIFI the kept network (N bytes) is the kernel's to join`
   - `wifi-net: ... the station joins it`

**Pass criteria, in order:**

1. associated: `THE NETWORK OF THE FILE IS JOINED`, or `wifi-net: ... joins it`, followed by link-up;
2. got an IP: `ifconfig en0` / configd shows a DHCP address. If there is no shell, look for IPConfiguration's lease
   line in the log;
3. the Settings Wi-Fi row shows the network's name;
4. traffic: ping the gateway, or an HTTP fetch (Safari opens a page). An NTP fix then also moves the RTC offset
   (`rtc: the system set the time`).

## Risks and remaining work (honest sizes)

- **DHCP depends on wifid** creating the en0 service. If wifid does not get that far, there is no IP even with a
  join. One fallback is a static service in configd's `preferences.plist`, seeded by init26 (about half a day). The
  other is a small DHCP client in init26 (about a day).
- **Unproven 26 requests:**
  - wifid may first ask for requests that are not answered yet (COUNTRY_CODE 51, LOCALE 28, roam and power
    profiles). The run's `apple80211: in this run:` summary lists them;
  - the TIMESTAMP base (`mach_continuous_time` here) is inferred.

  Expect one or two iterations, about half a day.
- **SPI interrupts:** still stubbed, so `wifi_irq=0` polls every millisecond. That is enough for a join and light
  traffic.
- **Security gaps:** WPA3-SAE and PMF are missing, and so are HakiOS review items W3, W4, W5 and W9 (no
  drop-unprotected rule, no group replay check, a ring pool that leaks on each re-join, teardown races).
- **The UFS fallback in early boot:** if `ufs` is not up when the radio asks for firmware, the radio reports "no
  fw/". As a workaround, `tools/wifi-run.sh` puts the firmware in the RAM disk anyway.
- **Optional:** an `AppleWLANDriver` service and a `WiFiHardwareHealthy=true` property in `hakios.wlan.cpp` would
  quiet wifid's watchdog/OCLA notifications. They are not required.
