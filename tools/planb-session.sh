#!/bin/sh
# plan B, the one initramfs debug-shell session (busybox ash), copied to /tmp of the initramfs with sda-head-64x4k.bin:
#   1 ext4 of pmOS_root shrunk to 19136251 blocks (e2fsck before and after)
#   2 sgdisk and its libraries copied out of the pmOS root into /tmp/sg (the initramfs has none)
#   3 the inner GPT of userdata rewritten for the future size (backup at the new end, p2 to the new last usable)
#   4 the outer GPT: userdata shrunk, darwin_var made; the original protective MBR kept; kernel re-read
#   5 checks; then the caller runs pmos_continue_boot
# Every step stops the script at the first failure ("STOP: ..."), leaving the shell for a manual fix.
set -u
NEW_USERDATA_BLOCKS=19261184
NEW_P2_BLOCKS=19136251
EXP_HEAD=d24813db760a58957735d6a75b6e42dd91e5c33fa986b5f0d5bad4f098b94c9c
EXP_TAIL=554b6ad33129eecb65cf624f1740ec498f023c5eeaa9b13dd8583910bc055636
P2_TYPE=B921B045-1DF0-41C3-AF44-4C6F280D3FAE
P2_UNIQ=467BD7D3-8CAA-4503-8E83-C34F5B97472C
OUTER='-d 33 -n 33:2967552:22228735 -t 33:1b81e7e6-f50d-419b-a739-2aeef8da3335 -u 33:76152dc1-6f89-b943-b73f-0f987c75a5ce -c 33:userdata -n 34:22228736:30617343 -t 34:48465300-0000-11aa-aa11-00306543ecac -u 34:7b9f2109-07d5-4cb6-941b-d974a2425d30 -c 34:darwin_var'
stop() { echo "STOP: $*"; exit 1; }
step() { echo "== $(cut -d' ' -f1 /proc/uptime)s: $*"; }

SAVED_HEAD=2d8c83528213d11e608ebc9259262afaf4e37efaf02334e8605e3209a9415c30
[ "$(dd if=/dev/sda bs=4096 count=64 2>/dev/null | sha256sum | cut -c1-64)" = "$SAVED_HEAD" ] || stop "the live GPT head is not the saved one"
dd if=/dev/sda bs=4096 count=1 of=/tmp/mbr.bin 2>/dev/null || stop "saving the MBR"
step "1 loop over userdata"
L=$(losetup --show -Pf --direct-io=on --sector-size 4096 /dev/sda33) || stop losetup
echo "loop $L"; sleep 1
[ -b "${L}p2" ] || stop "no ${L}p2"
step "1 e2fsck before"
e2fsck -f -y "${L}p2"; rc=$?; [ $rc -le 1 ] || stop "e2fsck rc=$rc"
step "1 resize2fs to $NEW_P2_BLOCKS"
resize2fs "${L}p2" $NEW_P2_BLOCKS || stop resize2fs
step "1 e2fsck after"
e2fsck -f -y "${L}p2"; rc=$?; [ $rc -le 1 ] || stop "e2fsck after rc=$rc"

step "2 sgdisk out of the pmOS root"
mkdir -p /tmp/sg /sysroot
mount -o ro "${L}p2" /sysroot || stop "mount ro"
for f in usr/bin/sgdisk lib/ld-musl-aarch64.so.1 lib/libuuid.so.1 lib/libpopt.so.0 lib/libstdc++.so.6 lib/libgcc_s.so.1; do
	cp "/sysroot/$f" /tmp/sg/ || stop "no /sysroot/$f"
done
umount /sysroot || stop umount
SG="/tmp/sg/ld-musl-aarch64.so.1 --library-path /tmp/sg /tmp/sg/sgdisk"
$SG --version || stop "sgdisk does not run"

step "3 inner GPT through a loop limited to $NEW_USERDATA_BLOCKS blocks"
L1=$(losetup --show -f --sector-size 4096 --sizelimit $((NEW_USERDATA_BLOCKS * 4096)) /dev/sda33) || stop losetup1
$SG -e -d 2 -n 2:124928:0 -t 2:$P2_TYPE -u 2:$P2_UNIQ -c 2:primary "$L1" || stop "inner sgdisk"
sync
$SG -v "$L1" | tail -3
$SG -p "$L1" | tail -3
$SG -i 2 "$L1" | grep -E 'First|Last|size|unique'
losetup -d "$L1"
L1=$(losetup --show -f -P --sector-size 4096 --sizelimit $((NEW_USERDATA_BLOCKS * 4096)) /dev/sda33) || stop losetup1b
sleep 1
N1=$(basename "$L1")
echo "kernel view at the new size: p1 $(cat /sys/class/block/${N1}p1/start) +$(cat /sys/class/block/${N1}p1/size), p2 $(cat /sys/class/block/${N1}p2/start) +$(cat /sys/class/block/${N1}p2/size) (512-sectors)"
[ "$(cat /sys/class/block/${N1}p2/size)" = $((NEW_P2_BLOCKS * 8)) ] || { losetup -d "$L1"; stop "p2 is not $NEW_P2_BLOCKS blocks in the kernel's view"; }
e2fsck -fn "${L1}p2" | tail -1 || { losetup -d "$L1"; stop "e2fsck -fn on the new p2"; }
losetup -d "$L1"
losetup -d "$L"
losetup -a

step "4 outer GPT of /dev/sda"
$SG $OUTER /dev/sda || stop "outer sgdisk"
dd if=/tmp/mbr.bin of=/dev/sda bs=4096 count=1 conv=notrunc 2>/dev/null || stop "MBR restore"
sync; blockdev --flushbufs /dev/sda 2>/dev/null
H=$(dd if=/dev/sda bs=4096 count=64 iflag=direct 2>/dev/null | sha256sum | cut -c1-64)
T=$(dd if=/dev/sda bs=4096 skip=30617536 count=64 iflag=direct 2>/dev/null | sha256sum | cut -c1-64)
[ "$H" = "$EXP_HEAD" ] && echo "HEAD_OK $H" || echo "HEAD_MISMATCH $H (want $EXP_HEAD)"
[ "$T" = "$EXP_TAIL" ] && echo "TAIL_OK $T" || echo "TAIL_MISMATCH $T (want $EXP_TAIL)"
$SG -v /dev/sda | tail -2
$SG -p /dev/sda | tail -3
blockdev --rereadpt /dev/sda 2>&1; sleep 2
if [ -e /sys/class/block/sda34 ]; then
	echo "kernel: sda33 +$(cat /sys/class/block/sda33/size), sda34 $(cat /sys/class/block/sda34/start) +$(cat /sys/class/block/sda34/size) $(grep PARTNAME /sys/class/block/sda34/uevent)"
else
	echo "KERNEL_HAS_OLD_TABLE: reboot -f instead of continuing"
	exit 2
fi
[ "$(cat /sys/class/block/sda33/size)" = $((NEW_USERDATA_BLOCKS * 8)) ] || stop "sda33 is not the new size in the kernel"

step "5 final look through a loop over the new sda33"
L=$(losetup --show -Pf --direct-io=on --sector-size 4096 /dev/sda33) || stop losetup2
sleep 1
N=$(basename "$L")
echo "p2 +$(cat /sys/class/block/${N}p2/size) (512-sectors, want $((NEW_P2_BLOCKS * 8)))"
e2fsck -fn "${L}p2" | tail -1
losetup -d "$L"
echo SESSION_DONE
