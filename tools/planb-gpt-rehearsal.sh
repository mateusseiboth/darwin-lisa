#!/bin/sh
# runs on pmOS as root: rehearse the outer GPT change on a sparse image carrying the live head and tail
set -eu
SG='-d 33 -n 33:2967552:22228735 -t 33:1b81e7e6-f50d-419b-a739-2aeef8da3335 -u 33:76152dc1-6f89-b943-b73f-0f987c75a5ce -c 33:userdata -n 34:22228736:30617343 -t 34:48465300-0000-11aa-aa11-00306543ecac -u 34:7b9f2109-07d5-4cb6-941b-d974a2425d30 -c 34:darwin_var'
IMG=/home/mateus/gptsim.img
rm -f $IMG
truncate -s $((30617600*4096)) $IMG
dd if=/dev/sda bs=4096 count=64 of=$IMG conv=notrunc 2>/dev/null
dd if=/dev/sda bs=4096 skip=30617536 of=$IMG seek=30617536 count=64 conv=notrunc 2>/dev/null
L=$(losetup --show -f --sector-size 4096 $IMG)
echo "sim loop $L"
sgdisk -p $L | tail -3
echo "---- apply"
sgdisk $SG $L; echo "rc=$?"
echo "---- verify"
sgdisk -v $L
sgdisk -p $L | tail -4
sgdisk -i 34 $L
sgdisk -i 33 $L | grep -E 'First|Last|GUID'
losetup -d $L
echo "---- kernel view"
L=$(losetup --show -Pf --sector-size 4096 $IMG); sleep 1
N=$(basename $L)
cat /sys/class/block/${N}p33/start /sys/class/block/${N}p33/size /sys/class/block/${N}p34/start /sys/class/block/${N}p34/size
grep PARTNAME /sys/class/block/${N}p34/uevent
losetup -d $L
dd if=$IMG bs=4096 count=64 2>/dev/null > /home/mateus/gptsim-head.bin
dd if=$IMG bs=4096 skip=30617536 count=64 2>/dev/null > /home/mateus/gptsim-tail.bin
sha256sum /home/mateus/gptsim-head.bin /home/mateus/gptsim-tail.bin
rm -f $IMG
