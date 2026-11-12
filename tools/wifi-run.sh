#!/usr/bin/env bash
# darwin-lisa: a host run of iOS from the flash with the Wi-Fi network given at the moment, never written to disk.
#
#   tools/wifi-run.sh              asks for the network's name and passphrase (not echoed), then runs
#                                  tools/run-flashroot.sh with them in a resident RAM disk
#
# The name and passphrase go into fw/network of a small APFS image (with the Wi-Fi firmware beside it) made in
# /dev/shm, which run-flashroot.sh uploads as the resident md0; the image is removed when this script ends. On the
# phone the kernel joins that network (hakios.wifi.c: fw/network of a RAM disk), and init26 keeps a copy on the
# data volume (/private/var/db/dlisa-wifi/network, 0600) so that later boots from the flash alone join it too
# (kernel/drivers/wifi-join/README.md). Nothing is written to this PC's disk, to the repository or to a log: the
# kernel and init26 print only lengths.
#
# Environment: everything run-flashroot.sh takes (KERNEL, XL_SECS, XL_TAG...), and
#   WIFI_ARGS      the Wi-Fi boot-args (default: wifi=1 wifi_irq=0 wifi_loud=1 wifi_mac=02:6c:69:73:61:01 rtc=1)
#   WIFI_FW_DIR    firmware put in the image beside fw/network (default userland-26/out/sysroot-26/fw; empty: none)
#   XL_EXTRA_ARGS  the rest of the boot-args (default: run-flashroot.sh's, plus the data volume's)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
MOBILE=/home/mateusseiboth/Dev/mobile
MKAPFS=$MOBILE/research/hackios-xnu-kernel/tools/apfs/mkapfs.py
FW=${WIFI_FW_DIR-$MOBILE/userland-26/out/sysroot-26/fw}
WIFI_ARGS=${WIFI_ARGS:-wifi=1 wifi_irq=0 wifi_loud=1 wifi_mac=02:6c:69:73:61:01 rtc=1}
DATA_ARGS="ufs_write=1 ufs_data=darwin_var var_ufs=1"

umask 077
WORK=$(mktemp -d -p /dev/shm wifi-run.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

read -r -p "Wi-Fi network name: " name
read -r -s -p "Passphrase (8 to 63 characters, empty for an open network): " passphrase
echo
(( ${#name} >= 1 && ${#name} <= 32 )) || { echo "wifi-run: the name must be 1 to 32 bytes" >&2; exit 1; }
(( ${#passphrase} == 0 || (${#passphrase} >= 8 && ${#passphrase} <= 63) )) ||
	{ echo "wifi-run: the passphrase must be 8 to 63 characters" >&2; exit 1; }

mkdir -p "$WORK/root/fw"
[[ -n $FW && -d $FW ]] && cp "$FW"/* "$WORK/root/fw/"
printf '%s\n%s\n' "$name" "$passphrase" >"$WORK/root/fw/network"
unset passphrase
python3 -I "$MKAPFS" -o "$WORK/net.apfs" "Net=$WORK/root" >/dev/null
rm -f "$WORK/root/fw/network"
truncate -s 32M "$WORK/net.apfs"
echo "wifi-run: network of ${#name} bytes in a resident RAM disk (/dev/shm, removed at the end); boot-args: $WIFI_ARGS"

EXTRA=${XL_EXTRA_ARGS-display=1 console_open=1 keystore=1 init_run=/usr/libexec/usermanagerd:--init init_run_secs=60 cpufreq=1 cpufreq_max=2400 cpus=7}
XL_FW_RAMDISK="$WORK/net.apfs" XL_EXTRA_ARGS="$DATA_ARGS $WIFI_ARGS $EXTRA" XL_TAG=${XL_TAG:-wifi-join} \
	"$HERE/run-flashroot.sh"
