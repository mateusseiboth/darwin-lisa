#!/bin/sh
# Boot the FULL iOS 26 root (system-26.apfs, ~3.6 GB) from a resident ramdisk.
# A fresh loader session is needed (a 2nd kernel run in one session dies), and a
# reboot wipes the resident region -- so the order is: reboot -> upload -> boot,
# with no reboot in between. Uses kernel26.nopac (Worker B's miniamfi build).
# Usage: ./run-fullroot.sh /abs/path/system-26.apfs
set -e
cd "$(dirname "$0")"
IMG="${1:?usage: run-fullroot.sh <system-26.apfs>}"
[ -f "$IMG" ] || { echo "image $IMG not found"; exit 1; }
# Default: the current build (16 KB pages since 2026-11-09). kernel-16k.bak is the
# verified 16 KB baseline; kernel-miniamfi.bak / kernel-userland.bak are 4 KB and
# need the 4 KB re-signed cache. Override with KERNEL=... if needed.
KERNEL="${KERNEL:-kernel26.nopac}"
[ -f "$KERNEL" ] || KERNEL=kernel26.nopac
[ -f "$KERNEL" ] || { echo "no kernel ($KERNEL)"; exit 1; }
SECS="${XL_SECS:-120}"
PHONE=${XDG_RUNTIME_DIR:-/tmp}/darwin-hakios-phone
cp "$KERNEL" hakios-kernel

echo "### fresh loader session (reboot wipes the resident region)"
fastboot reboot >/dev/null 2>&1 || true
for i in $(seq 1 30); do lsusb 2>/dev/null | grep -q 18d1:d00d || break; sleep 1; done
for i in $(seq 1 60); do timeout 5 fastboot getvar product 2>&1 | grep -qi xnuloader && break; sleep 1; done
timeout 6 fastboot getvar product 2>&1 | grep -qi xnuloader || { echo "loader not up"; exit 1; }
rm -f "$PHONE.ran"

echo "### upload resident root ($(( $(stat -c %s "$IMG") / 1048576 )) MB) via xl (proven path)"
t0=$(date +%s)
XL_RAMDISK=resident ./xl ramdisk "$IMG" || { echo "resident upload failed (window too small?)"; exit 2; }
echo "### uploaded in $(( $(date +%s) - t0 ))s"

echo "### boot kernel26.nopac (no ramdisk=; loader prepends cpus=1 rd=md0, wires resident md0)"
xnuloader/mkpayload.py -o run-payload.bin kernel=hakios-kernel \
  --cmdline "loader_return_after=$SECS epoch=$(date +%s) sb_delay=8 hv_wdt_pet=1 bootcpu=7 debug=0x8 -v ${XL_EXTRA_ARGS:-}" >/dev/null
timeout 60 fastboot stage run-payload.bin >/dev/null 2>&1
timeout 20 fastboot oem run >/dev/null 2>&1

best=0
for i in $(seq 1 ${XL_POLLS:-60}); do
  timeout 5 fastboot oem log >/dev/null 2>&1 || true
  if timeout 8 fastboot get_staged /tmp/fr.$i >/dev/null 2>&1; then
    n=$(tr -d '\000' < /tmp/fr.$i 2>/dev/null | wc -c)
    [ "$n" -ge "$best" ] && best=$n && tr -d '\000' < /tmp/fr.$i > logs/fullroot.log
  fi
  r=$(timeout 5 fastboot getvar result 2>&1 | grep -i '^result:' || true)
  case "$r" in *returned*|*exception*|*checkpoint*) break;; esac
  sleep 2
done
echo "---- kernel portion (best live snapshot, $best bytes) ----"
sed -n '/KERNEL LOG\|jumping to/,$p' logs/fullroot.log | grep -vE '^	  (x[0-9]|fp|pc)' | tail -70
echo "---- root-mount + init markers ----"
grep -nE "BSD root|md0|apfs: volume|RAMDisk|launchd alive|init26:|contents of /|Applications|System|private|usr|handing over|cannot exec|panic|Debugger" logs/fullroot.log | tail -40
