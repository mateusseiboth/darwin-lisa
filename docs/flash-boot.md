# Flash boot: the OS switch over USB, and the iOS root on lisa's own UFS

Everything here was prepared offline. Nothing has been flashed or run on the phone. Each device step is tagged:

| Tag | Meaning |
|---|---|
| **[READ-ONLY]** | reads only; the agent can run it |
| **[RAM-ONLY]** | boots or loads something into RAM; nothing on the flash changes; the agent can run it |
| **[WRITES: X, APPROVAL]** | writes partition X; stop and get the user's approval at that moment |
| **[USER PRESENT]** | needs a key press on the phone |

Hard rules for every step: never touch slot b, never `--slot all`, never `set_active`, leave vbmeta alone.

## 1. What was built

| Artifact | sha256 (first 16) | What |
|---|---|---|
| `out/u-boot-lisa-ums.img` | `a1f926c019727047` | `u-boot-lisa-ossel.img` + ums that says why it ended (`fastboot getvar ums`), stays in U-Boot fastboot when it fails, and ends only on a real eject. Same `.config` |
| `out/u-boot-lisa-ossel.img` | `796474c9b931dc67` | U-Boot boot image (v3, gzip kernel, no ramdisk). Same `.config` as the flashed one; only the environment differs: `osel_check` and the "USB disk" menu entry |
| `out/vendor_boot-lisa-ossel.img` | `633f56a0a7a93aa3` | `vendor_boot-multiboot-lisa.img` with only the vendor ramdisk replaced (the HKUB with the patched loader). Header and DTB bytes are identical, so pmOS gets the same DTB |
| `out/loader-usb-lisa-ossel.hkub` | `526a17a760057bec` | the same HKUB on its own, for U-Boot's "HakiOS over fastboot" (RAM only) |
| `out/loader-ossel.efi` | `902302397c9efae7` | the patched loader |
| `out/u-boot-lisa-multiboot-current.img` | `9a0f13b4db8f50c1` | **rollback for boot_a**: the multiboot U-Boot (default HakiOS) flashed on 2026-11-08 11:36 |
| `research/hackios-xnu-kernel/out/vendor_boot-multiboot-lisa.img` | `c4d960ba756dc1c6` | **rollback for vendor_boot_a** |
| `out/pmos-lisa-r17-20261110/` | see its SHA256SUMS | **rollback for pmOS**: `xiaomi-lisa.img` (userdata image), boot, vendor_boot, dtbo |
| `stock/extract/.../images/super.img` | | **rollback for super** (Android OS2.0.8.0, sparse) |

Note: `out/u-boot-lisa.img` is NOT the flashed U-Boot. It is an older mainline build (`U-Boot 2025.04-rc1-g0532f74d`,
header v0, `bootcmd=bootefi bootmgr`, no menu, no hakios command). Don't use it as a rollback.
`research/hackios-xnu-kernel/out/boot-multiboot-lisa.img` is the same multiboot U-Boot with `bootmenu_default 0`
(pmOS); it was replaced at 11:36 by the default-HakiOS build, now saved as `out/u-boot-lisa-multiboot-current.img`.

Sources:

- `u-boot-lisa/board/qualcomm/hakios-lisa.env`: `osel_check`, `osel_1..4`, `ums_disk`, menu entry 5. Build with
  `u-boot-lisa/build.sh`, which uses a private copy of HakiOS's U-Boot tree, so the shared tree is never touched.
- `darwin-lisa/loader/oem-bootos.patch` (against `research/hackios-xnu-kernel`, `git apply --check` passes) and
  `darwin-lisa/loader/build-ossel.sh`, which patches and builds a copy and leaves `xnuloader/` alone.
- `darwin-lisa/tools/bootos.sh`, `abl-fastboot.sh`, `ums-guard.sh`, `darwin-write.py`, `lpdump.py`.
- `darwin-lisa/kernel/drivers/ufs/` is the UFS driver for xnu-12377 (`ufs-wire.py`, not applied; see its README).

## 2. How the OS switch works, and why not a memory cookie

**A cookie that has to survive a reset doesn't work on lisa.** ABL's own logs (`UefiLog0-4.txt` from `logfs`) show
every boot, including software reboots, as `PM: Reset by PSHOLD`, `PM: Reset Type: Hard Reset`, `PON Reason ...
cold_boot:1`, and XBL runs `Pimem init` on each one. A PMIC hard reset is not a warm reset, so neither DDR nor IMEM
can be trusted to keep a value. The vendor IMEM restart-reason word is at `0x146aa65c`
(`qcom,msm-imem@146aa000` / `restart_reason@65c`), but ABL takes its reboot reason from a PMIC SDAM nvmem cell
(`qcom,reboot-reason`), not from IMEM. U-Boot also has no `fastboot_set_reboot_flag` for snapdragon
(`DM_REBOOT_MODE` off), so it can't set a reason either.

**What was built instead needs no reset at all.** The loader is a UEFI app started by U-Boot (`hakios boot` =
`bootefi`).

1. On `fastboot oem bootos:TARGET` (pmos, ums, uboot or menu), and only if no kernel has run in this loader
   session, the loader:
   - stops the hypervisor watchdog it armed (`VIRT_WDT_CONTROL 2`, the Linux hh_virt_wdt "disable"), checks with
     `VIRT_WDT_STATUS` that it really stopped, and if not, re-arms it and answers FAIL;
   - writes `"OSEL"`, the code, and `~code` at `0xf3fff000`, the last page of the 64 MiB payload window that the
     DT reserves (`hakios-payload@f0000000`). The loader uses only up to `0xf3f80000` there, and nothing in U-Boot
     allocates in the window;
   - answers OKAY, releases its USB buffers, calls `USB->stop()` (U-Boot's `qusb_stop`, which unregisters the
     gadget so ums or fastboot can take the UDC), and calls `BS->Exit`.
2. U-Boot returns from `hakios boot` into `osel_check`. It reads the three words, clears them, and runs `osel_N`.
   An unknown code or a bad check word does nothing, and a failed target never loops.
3. On a cold boot the request is never read, because `osel_check` runs only after the loader returns, and the
   loader zeroes the page when it starts. So: menu, 5 s, HakiOS, exactly as before. Volume keys are unchanged.
   `reboot` from pmOS lands in HakiOS. ABL's own `reboot bootloader` path is untouched, since nothing here writes
   PON/SDAM or IMEM.
4. If a kernel has run in the session, or one is running, `bootos.sh` sends `oem stop` and `fastboot reboot` first,
   and asks a fresh loader. A kernel leaves the hardware as it likes, so U-Boot never inherits that state.

`osel_check` was tested in a U-Boot sandbox build from the same tree: a request runs the target once and is cleared,
it never runs again, a bad check word is ignored, and an unknown code just prints `osel_7 not defined` and falls
through.

**The USB disk (`ums 0 scsi 0`)** is mainline U-Boot's standard Qualcomm-phone entry (`qcom-phone.env` has the same
line). The tree's f_mass_storage handles 4096-byte LUNs (`blkbits` from the block size), and the UFS uclass sends
WRITE10 with `UTP_HOST_TO_DEVICE`. U-Boot's own `fastboot flash` only knows MMC, so ums is the PC's write path. Reads
are validated first (step B2). The first real write is the image itself, and it is read back in full.

On the PC the disk is vendor `Linux`, model `UMS disk 0` (`bootos.sh` and `ums-guard.sh` match that; the
`File-Store Gadget` string in f_mass_storage is never sent). With `u-boot-lisa-ums.img`, `ums_disk` restarts the
phone only after an eject; a key opens the menu; any other end leaves U-Boot's fastboot up and `fastboot getvar ums`
says why (`init`, `udc -19`, `gadget -16`, ...). `bootos.sh ums` prints that reason. The first `bootos.sh ums` with
`u-boot-lisa-ossel.img` (2026-11-10 09:34) never showed a USB device on the PC: the loader left at 09:34:25 and was
back at 09:34:36, a full reset, so `ums` failed at once and `ums_disk` reset.

## 3. Device sequence

### A. The OS switch (U-Boot + loader)

**A0 [READ-ONLY] confirm what is flashed now** (from pmOS, user `mateus`; the password comes from `PMOS_PASS`,
never from a file):

```sh
ssh mateus@172.16.42.1 'sudo head -c 761856 /dev/disk/by-partlabel/boot_a | sha256sum;
                        sudo head -c 360448 /dev/disk/by-partlabel/vendor_boot_a | sha256sum'
# boot_a must be        9a0f13b4db8f50c1c4c177510492e9d52ae8cca3fed534aab5818dec3371c2b7 (out/u-boot-lisa-multiboot-current.img)
# vendor_boot_a must be c4d960ba756dc1c60af501ec911e11b8efaa9be73536d3abc92a0d22c4ba0695 (vendor_boot-multiboot-lisa.img)
```

If either differs, stop: the rollback files are not what is on the phone. Dump the real ones first
(`sudo dd if=... bs=1M`, read-only) and use those as the rollback.

**A1 [RAM-ONLY] reach ABL fastboot that accepts transfers, no keys:** `darwin-lisa/tools/abl-fastboot.sh`. It goes
through pmOS (`reboot bootloader`), then runs `fastboot reboot bootloader` a second time, and proves acceptance with
a 4 KiB `fastboot stage` (RAM). If that fails it asks for Power + Volume Down **[USER PRESENT]**.

**A2 [RAM-ONLY] test the new U-Boot from RAM:**
`fastboot boot out/u-boot-lisa-ossel.img`. ABL keeps using the flashed vendor_boot_a, so the loader is still the
old one. Expect the same menu, 5 s, then HakiOS, with `fastboot getvar product` = `xnuloader`. The old loader answers
`oem bootos:` with `FAIL unknown command`, which is expected here.
`fastboot boot` on lisa's ABL has been flaky ("Sending" hangs). Retry, or go to A3 with the rollback at hand.

**A2b [RAM-ONLY, USER PRESENT, optional] test the new loader before flashing it:** in the RAM-booted U-Boot pick
"HakiOS over fastboot" with the volume keys, then:

```sh
fastboot stage out/loader-usb-lisa-ossel.hkub
fastboot oem run:"hakios load fastboot"
fastboot continue                      # the new loader comes up: product xnuloader
fastboot oem bootos:menu               # exits to U-Boot: the menu shows without a reset
```

Then pick "USB disk for the PC" to test ums read-only (B2), or "Reset device".

**A3 [WRITES: boot_a, APPROVAL]** from ABL fastboot (A1): `fastboot flash boot_a out/u-boot-lisa-ossel.img`, then
`fastboot reboot`. Or from pmOS (the usual way): `scp` the image, then
`sudo dd if=/tmp/u-boot-lisa-ossel.img of=/dev/disk/by-partlabel/boot_a bs=1M conv=fsync`. Read it back with
`sudo head -c 761856 ... | sha256sum`. Rollback: the same with `out/u-boot-lisa-multiboot-current.img`.

**A4 [WRITES: vendor_boot_a, APPROVAL]** the same way with `out/vendor_boot-lisa-ossel.img` (`of=.../vendor_boot_a`,
compare the first 360448 bytes). Rollback: `research/hackios-xnu-kernel/out/vendor_boot-multiboot-lisa.img`. Only
the 151552-byte vendor ramdisk at 0x1000 differs, so pmOS's DTB is the same bytes.

**A5 [RAM-ONLY] check it:**

```sh
tools/bootos.sh state         # loader
tools/bootos.sh pmos          # pmOS SSH answers within ~60 s
tools/bootos.sh hakios        # sudo reboot over SSH -> menu -> loader (needs PMOS_PASS or a NOPASSWD rule)
tools/bootos.sh ums           # prints /dev/sdX; then: sudo eject /dev/sdX -> the phone restarts into HakiOS
tools/bootos.sh uboot         # U-Boot's fastboot; fastboot reboot -> HakiOS
```

Also check, with the user if possible: a cold power-on still shows the 5 s menu and lands in HakiOS, and Volume
Down still opens the menu.

### B. Read-only survey (decides the branch)

**B1 [READ-ONLY] from pmOS:**

```sh
findmnt / /boot; lsblk -f; sudo blkid; losetup -a; cat /proc/cmdline; df -h /
ls -l /dev/disk/by-partlabel/ | grep -E 'super|userdata|darwin|system'
```

Expected: `/` is ext4 UUID `2e1dd6c6-7179-49d8-b3a4-5bfcd397b04d` and `/boot` is ext2 `d0ff3185-...`, both on a loop
device whose backing file is `/dev/sda33` (userdata). `sda20` is `super`, which matches the 2026-11-08 GPT backup and
the pmOS deviceinfo `super_partitions=/dev/sda20`.

**B2 [READ-ONLY] the GPT and super from the PC** (`tools/bootos.sh ums` gives `/dev/sdX`):

```sh
sudo tools/ums-guard.sh /dev/sdX                  # every partition read-only in the PC's kernel, udisks told to ignore it
sudo sgdisk -p /dev/sdX
sudo sgdisk --backup=backups/lisa-gpt-sda-$(date +%Y%m%d).bin /dev/sdX
sudo python3 -I darwin-lisa/tools/darwin-write.py check --disk /dev/sdX --part super --save backups/lisa-identity-$(date +%Y%m%d).json
sudo python3 -I darwin-lisa/tools/lpdump.py /dev/sdX20 --super-first-lba 573440        # what Android has in super
L=$(sudo losetup -r -P -b 4096 --show -f /dev/sdX33); sudo sgdisk -p $L; sudo blkid ${L}p1 ${L}p2; sudo losetup -d $L
sudo eject /dev/sdX                                # the phone restarts into HakiOS
```

Offline, the 2026-11-08 GPT backup (`backups/lisa-pmos-20261108/gpt-sda-*.bin`) shows 33 entries on sda and **no
`darwin` partition**. userdata (33) runs from LBA 2967552 to the disk's last usable LBA 30617594 (105.5 GiB).
super (20) is LBA 573440..2801663 (8.50 GiB). The stock super's LP metadata (`lpdump.py` on the factory super.img)
holds the `_a` partitions only: odm, product, system (0.75 GiB), system_ext, vendor and mi_ext, 6.44 GiB in all.
Every `_b` is 0 bytes and 2.06 GiB is free. **system_b is empty**, so it was never an option.

**Decision:**

| B1/B2 show | Branch |
|---|---|
| a GPT entry named `darwin` | 1: write into `darwin` (`--part darwin --expect <saved json>`) |
| no `darwin`; pmOS root and boot inside userdata's loop; super holds LP metadata (Android) and no partition table | **2b: the raw `super`** (preferred; the user agreed to remove Android) |
| pmOS in super, or super unusable | 3: shrink userdata, create `darwin` |
| pmOS NOT in userdata | userdata itself is free for the root, with no resize (unexpected; say so to the user first) |

### C. Write the root (branch 2b: super)

Sizes: the full image is built from the current sysroot with `FULL=1 userland-26/scripts/mkimage-trim.sh`
(`userland-26/out/system-26.apfs.full`: nothing left out, Applications/ and PosterBoard included, padded to a 32 MiB
multiple). On 2026-11-10 it is 4,831,838,208 B (144 x 32 MiB, sha256 `195ae6b547baf4c8...`), which fits in super
(2,228,224 blocks) and leaves 4.0 GiB after it. (`system-26.apfs.new` is stale: from before pacstrip/unzbm/init/inject.) The trimmed image (3.75 GiB) would leave 4.75 GiB. Use the full one.
Since the flash root never passes through fastboot, the 4 GiB download cap doesn't apply. The data volume stays on
ramfs at first; the 4.0 GiB tail of super can later hold a persistent `/private/var` (kernel `ufs_write=1
ufs_write_first=1177993`).

Nothing in the remaining boot chain reads super:

- ABL loads boot_a, vendor_boot_a, dtbo and vbmeta_a only. Its log shows `vbmeta_a: ... OK_NOT_SIGNED` and
  `boot state is: orange`, so the boot goes on unverified. vbmeta's hashtree descriptors for system/vendor are used
  by Android's first-stage init, which no longer runs.
- U-Boot reads userdata (pmos_boot) and the boot image ramdisk.
- The loader reads `logdump` and the GPT.
- pmOS's initramfs only probes partitions that carry exactly 2 subpartitions, and a raw APFS image has none.

Restoring Android later means flashing the stock `super.img`.

Keep the GPT name `super`. A rename to `darwin` would be a small GPT write, but it buys nothing: the kernel selects
the partition by name (`ufs_part=super`), Xiaomi's `flash_all.sh` and ABL fastboot expect `super`, and every GPT
write carries risk.

**C1 [READ-ONLY] save today's super on the PC** (8.5 GiB, the host has ~96 GB free):

```sh
sudo dd if=/dev/sdX20 of=backups/lisa-super-$(date +%Y%m%d).img bs=8M status=progress && sha256sum backups/lisa-super-*.img
```

**C2 [WRITES: super, APPROVAL]:**

```sh
sudo tools/ums-guard.sh /dev/sdX 20              # only sdX20 writable
sudo python3 -I darwin-lisa/tools/darwin-write.py plan  --disk /dev/sdX --part super --expect backups/lisa-identity-*.json \
     userland-26/out/system-26.apfs.full        # all checks, nothing written
sudo DARWIN_WRITE=yes python3 -I darwin-lisa/tools/darwin-write.py write --disk /dev/sdX --part super \
     --expect backups/lisa-identity-*.json userland-26/out/system-26.apfs.full
sudo eject /dev/sdX
```

`write` checks the GPT, the partition's identity, the sysfs node, and that nothing holds the partition. It refuses
if super holds a partition table or ext4, writes only through `/dev/sdX20`, then drops the cache, reads the whole
range back and compares sha256. Expect about 2-4 minutes over USB 2. The log is `darwin-lisa/logs/darwin-write.log`.

Fallback if ums doesn't work: from pmOS, `scp` the image to `/home/mateus/` (userdata has room). Then run
`sudo dd if=system-26.apfs.full of=/dev/disk/by-partlabel/super bs=8M conv=fsync` and
`sudo head -c 4831838208 /dev/disk/by-partlabel/super | sha256sum`. If pmOS has python3, run `darwin-write.py`
there with `--disk /dev/sda`.

### D. Boot the root from flash (after the kernel owner frees the tree)

**D1 [build, not a device step]** in docker: `python3 darwin-lisa/kernel/drivers/ufs/ufs-wire.py` (after
`hakios-wire.py`), then `.work/run-rebuild.sh`, then grep `logs/kernel.log` for `error:`.

**D2 [RAM-ONLY]** read-only boots, with no `xl ramdisk` upload, so the 3.84 GB of RAM is freed. Each test adds to
the usual line:

1. `ufs=2`: the partition table (`/dev/ufs_super`); root still md0 if one is uploaded.
2. `rd=md0 ufs=2 ufs_part=super ufs_root=1`: expect `BSD root: ufs_super`, then `apfs: volume 0 "System"`.
   Keep `rd=md0`: without it, bsd_init's seal check panics and launchd leaves Restore mode. The loader adds it only
   when it uploads a RAM disk, so put it in `XL_CMDLINE` yourself.
3. The same plus `ufs_native=1` (31 requests in flight instead of one through the loader mailbox).

`darwin-lisa/tools/run-flashroot.sh` drives test 2: a fresh loader session, no `xl ramdisk`, the UFS root arguments
plus the current speed arguments, the live log in `logs/flashroot.log`, a screenshot every 60 polls
(`logs/flashroot-shot-*.png`) and the `[MEM]` lines. `XL_UFS_ARGS` / `XL_EXTRA_ARGS` override the arguments.

Multi-core: the mailbox needs the loader alive on core 0 (`bootcpu=7`, so `cpus` is at most 7). If the kernel takes
core 0, use `ufs_native=1 ufs_loader=0` instead (read-only, untested).

### Branch 3 (fallback): shrink userdata and create `darwin`

Use this only if super is unusable. Everything runs on the PC through ums, with pmOS offline (ext4 can't shrink while
mounted). Proposed size: **32 GiB**. That holds the 4.5 GiB image, room for a bigger image or a second copy to
update into, and a ~20 GiB data volume, and leaves userdata ~73.5 GiB. With today's table that is darwin =
LBA 22228736..30617594 (1 MiB aligned) and userdata = 2967552..22228735. Recompute from the live GPT.

Before you start, check how full pmOS is: `df -h /` in B1. The local chroot is not a usable hint (bind mounts).

1. **[READ-ONLY]** B2 dumps, plus `sudo e2fsck -fn ${L}p2` on a read-only loop.
2. **Backup:**
   - `out/pmos-lisa-r17-20261110/xiaomi-lisa.img` restores a fresh pmOS: `fastboot flash userdata`, which ERASES
     userdata. **[WRITES, APPROVAL]**
   - To keep the user's pmOS state, also image the used part of its root:
     `sudo e2image -ra -p ${L}p2 backups/pmos-root-$(date +%F).e2i`, on a read-only loop.
3. **[WRITES: userdata (inner fs), APPROVAL]** `L=$(losetup -P -b 4096 --show -f /dev/sdX33)`, then
   `e2fsck -f ${L}p2`, then `resize2fs ${L}p2 <new size - 64M margin>`.
4. **[WRITES: userdata (inner GPT), APPROVAL]** detach, then re-attach the loop limited to the new userdata size
   (`losetup --sizelimit $((19261184*4096)) -P -b 4096 ...`). Then `sgdisk -e $L` (moves the inner backup GPT to the
   new end), and `sgdisk -d 2 -n 2:<same start>:0 -t 2:<same type> -u 2:<same unique GUID> -c 2:pmOS_root $L`. Then
   `resize2fs ${L}p2` (grows to fit exactly) and `e2fsck -f ${L}p2`.
5. **[WRITES: sda GPT, APPROVAL]** `sgdisk -d 33 -n 33:2967552:22228735 -t 33:<type> -u 33:76152dc1-... -c 33:userdata`,
   then `-n 34:22228736:30617594 -t 34:7C3457EF-0000-11AA-AA11-00306543ECAC -c 34:darwin`. sgdisk writes the
   primary and backup tables. Then `sgdisk -v` and `partprobe`.
6. **[READ-ONLY]** `e2fsck -fn` again. Boot pmOS (`bootos.sh pmos`): `pmos_boot` uses
   `part start/size scsi 0 userdata`, so a smaller userdata stays consistent. pmOS's initramfs runs
   `parted -f resizepart 2 100%` on each boot, which is a no-op when p2 already fills the image.

GPT validation: HakiOS's redwood (same SoC, same Qualcomm ABL family) ran with an extra `darwin` entry after
userdata on LUN 0. sda here has 64 entry slots, 33 used. ABL and XBL look partitions up by name, and sgdisk keeps
both CRCs right.

**Last resort, not prepared:** a preallocated image file inside pmOS's ext4, passed to the kernel as an extent list.
It needs a new kernel device and FIEMAP offsets nested under the inner GPT and userdata, and pmOS must never move the
file.

## 4. Rollbacks

| Partition | Rollback | How |
|---|---|---|
| boot_a | `out/u-boot-lisa-multiboot-current.img` | pmOS `dd` or ABL `fastboot flash boot_a` |
| vendor_boot_a | `research/hackios-xnu-kernel/out/vendor_boot-multiboot-lisa.img` | the same |
| super | `backups/lisa-super-<date>.img` (C1) or the stock `super.img` | ums `dd`, or ABL `fastboot flash super` |
| userdata (branch 3) | `out/pmos-lisa-r17-20261110/xiaomi-lisa.img` + the e2image; the sgdisk `--backup` file | `sgdisk --load-backup` |

## 5. Open questions

- Watchdog disable value: `VIRT_WDT_CONTROL 2` comes from Linux's hh_virt_wdt, not from a lisa trace. The loader
  verifies it through `VIRT_WDT_STATUS`, falls back to `0`, and refuses to exit if neither works, so a wrong guess
  costs a FAIL, not a reset.
- `BS->Exit` from the loader has never run on this U-Boot. If U-Boot misbehaves after it, A2b shows it before
  anything is flashed.
- ums write throughput, and UFS writes from U-Boot, are untested on this board. The first write is verified in full.
- Only the 2026-11-08 GPT backup exists offline. B2 must confirm it is unchanged.
- The trimmed RAM image was padded to a 32 MiB multiple (a UBSAN trap). super is exactly 272 x 32 MiB, but the full
  image is not a 32 MiB multiple. Watch D2 for that trap.
- The kernel UFS port was reviewed statically only, never compiled.
