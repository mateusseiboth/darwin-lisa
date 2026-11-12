# Data volume: a persistent /private/var on lisa's flash

Today `/private/var` is ramfs, so it starts empty on every boot and the first-boot work (LaunchServices seeding,
migrations) runs again each time. With the full 252-app root, the progress bar was still at about 85% after 18.7 min.
This puts `/private/var` on the flash so that work happens once.

Everything here was prepared offline. Nothing has been flashed, built in docker or run on the phone. Tags used
below, as in `flash-boot.md`:

| Tag | Meaning |
|---|---|
| **[READ-ONLY]** | reads only |
| **[RAM-ONLY]** | boots or loads into RAM; the flash does not change |
| **[WRITES: X]** | writes X. The user authorized partition writes; the step still says exactly what is written |

Hard rules: never touch slot b, vbmeta, persist, modem or the other firmware partitions. Never write the sda GPT
outside the explicit B steps (section 7).

## 1. Design

```
super (sda20, LBA 573440..2801663, 2228224 blocks of 4096)
 0 ............................ 1179648 ................................ 2228224
 | iOS root, APFS, read only     | data volume: HFSX, journaled, 4 GiB     |
 | (system-26.apfs.full)         | /dev/ufs_data -> /private/var           |
 | 4,831,838,208 B               | 4,294,967,296 B, LBA 1753088..2801663   |
```

Three layers each refuse every write outside the region. All three take the region from the same definition, a
GPT name plus an exact block range:

1. **Loader** (`loader/data-region.patch` + `loader/data-regions.h`). A write is served only when the run asks
   for it (`ufs_write=1 ufs_data=NAME`) and the partition named NAME matches its `data-regions.h` line exactly: unit 0,
   type GUID, unique GUID, first and last LBA, 4096-byte blocks, and only one partition with that name. Writes
   then go only to blocks `[write_first, write_end)` of that partition. With `root_below` (option A) the loader also
   reads the APFS superblock at super's block 0 on every run, and refuses if the container is missing or reaches
   past `write_first`. Every other request answers `EFI_ACCESS_DENIED`. The page gets a new field, `write_end`,
   in what used to be padding, so the page keeps its size and version.
2. **Kernel** (`kernel/drivers/ufs/datavol.patch`). `/dev/ufs_data` is a block device covering exactly the region
   the loader offers. The kernel checks the same name, unit 0, block size, the loader's offer, and a nonzero
   `write_end`. When the region shares the root's partition (A), the floor must not be 0, and the kernel reads the
   root container's superblock again. `ufs_write()` refuses anything outside `[write_first, write_end)`. Character
   devices no longer write at all, so HakiOS's packed store (`var_store`) is gone. With `ufs_native=1`, the kernel's
   own transfer list goes back into the controller `ufs_data_restore_ms` (default 2000) after the last write. Before
   this change, it stayed out for the rest of the run.
3. **UEFI's driver** bounds each request by the partition's own BlockIo, as before.

Options A and B use the same loader and kernel code. Only the `data-regions.h` line and `ufs_data=NAME`
differ.

**The filesystem is HFSX (journaled, case-sensitive HFS Plus), not APFS.** This kernel has no Apple apfs kext. Its
APFS is HakiOS's own port (`bsd/miscfs/apfs`), which always mounts `MNT_RDONLY` and refuses a write upgrade with
`EROFS`. Its `apfs_write.c` only rewrites the contents of existing files in place. HakiOS 16.5 hit the same
problem and used HFS Plus. It built Apple's open-source `hfs-627.100.6` into the kernel and ran a journaled HFSX
volume of about 12 GB on `/dev/ufs_darwin_data`, kept between boots (its CHANGELOG, `userland/src/init.c`
`var_hfs=`). Our `/dev/ufs_data` is that device with tighter bounds. Our `mkapfs.py` cannot make a Data-role
volume either: it writes read-only System-style volumes and sets no role. Because the boot-args, init26 code and
image tool are filesystem-agnostic, an APFS volume can replace HFS once someone writes a read-write APFS.

**init26** (`userland/init/init-26.c`), with `var_ufs=1`, mounts `hfs` from `/dev/ufs_data` on `/private/var`. If
there is no device or the mount fails, it falls back to ramfs exactly as before. On the flash volume it:

- empties `tmp`, `run` and `vm`, which belong to each boot, as HakiOS did;
- lays down only the missing skeleton entries (`var-skel.h`, with owners and modes), so the first boot of an empty
  volume gets all of it and later boots change nothing;
- creates the logd `.migrated` markers and `/private/var/keybags` if they are missing;
- starts a child that calls `sync()` every `var_sync=` seconds (default 30).

A run usually ends in a reset rather than an orderly shutdown, so this bounds what is lost. The HFS journal
keeps the volume consistent either way.

### Boot-args

| boot-arg | who reads it | what |
|---|---|---|
| `ufs_write=1` | loader, kernel | writes may be asked for this run. Without it, nothing writes |
| `ufs_data=NAME` | loader, kernel | the region: `super` (A) or later `darwin_var` (B). Must match a `data-regions.h` line |
| `var_ufs=1` | init26 | mount `/dev/ufs_data` (HFS) on `/private/var`; ramfs if that fails |
| `var_sync=N` | init26 | sync every N s (default 30; 0 = never) |
| `var_grow=1` | init26 | grow the volume to the whole device once (A to B migration, section 7) |
| `var_migrate=1` | init26 | do not write DataMigrator's "already migrated" record (default: written at every boot) |
| `ufs_data_restore_ms=N` | kernel | with `ufs_native=1`: when the kernel's list goes back in after writes (0 = end of run) |
| `ufs_write_first=N` | kernel | can only raise the floor; normally not needed |
| `ls_reset=1` | init26 | LaunchServices' stores, installd's markers and the MobileGestalt cache go; apps and extensions register again this boot |
| `cc_reset=1` | init26 | `mobile/Library/ControlCenter/*` goes (ControlsIconState, ModuleConfiguration, ControlsMigrationSettings): a layout saved while no module or control was registered is empty and stays so; the default controls are laid out again |
| `poster_reset=1` | init26 | `mobile/Library/Application Support/PRBPosterExtensionDataStore/*` and PosterBoard's defaults go; PosterBoard sets the default posters up again (PBFInitialPosterHelper) |

The A line, added to the usual flash-root arguments:

```
rd=md0 ufs=2 ufs_part=super ufs_root=1 ufs_write=1 ufs_data=super var_ufs=1
```

## 2. What was built (offline)

| Artifact | sha256 | What |
|---|---|---|
| `out/vendor_boot-lisa-datavol.img` | `7c1f3182f0501…` | vendor_boot = `vendor_boot-multiboot-lisa.img` with the vendor ramdisk replaced by the datavol HKUB. The new loader is one page longer, so the image was repacked (v3 layout): the header differs only in `vendor_ramdisk_size` (byte 25), and the DTB bytes are identical, one page further in. 364,544 B |
| `out/loader-usb-lisa-datavol.hkub` | `1cec2de075470…` | the same HKUB alone, for "HakiOS over fastboot" (RAM) |
| `out/loader-datavol.efi` | `eecdca8510de5…` | the loader: oem-bootos + data-region |
| `out/datavol/var-A.hfsx` (+ `.gz`, `.sha256`) | raw `63d252f16078e5f4…`, gz `78bd9ce99bc9…` | the empty 4 GiB HFSX volume (1048576 blocks of 4096, journal 8 MB, root 0:0 0755). `fsck.hfsplus -fy` finds it OK |

Sources:

- `loader/data-region.patch` (applies after `oem-bootos.patch`), `loader/data-regions.h`, and
  `loader/build-ossel.sh datavol`. The default (`ossel`) flavor is unchanged. Its build was rerun once to check
  and the original outputs were restored, because the loader build is not byte-reproducible (the PE timestamp).
- `kernel/drivers/ufs/datavol.patch` (against `hakios.ufs.c`), `datavol-wire.py`, and `hfs_compat_12377.h`.
- `tools/mkvarvol.py` builds the image, and `tools/var-write.sh` writes it from pmOS.
- `userland/init/init-26.c`, mirrored to `userland-26/init/init-26.c`.

Offline checks that were run:

- The loader compiles with no warnings, and `ufs_write=1 ufs_data=super` produces the "may be written this run,
  nothing else" string.
- `hakios.ufs.c`, with and without the patch, passes a syntax check with the kernel's own compile line (from
  `hakios.ufs.o.json`, host clang 22). It gives 0 errors and the same warning set.
- All 38 files of `bsd/hfs` compile against this tree's `EXPORT_HDRS` with HFS's flags. Only two KPIs were missing,
  `nspace_snapshot_event` and `resolve_nspace_item`, and `hfs_compat_12377.h` covers them. Every undefined symbol
  of the HFS objects is defined in `kernel26.nopac`.
- `datavol-wire.py` ran twice on a scratch copy of the tree's files (the second run skips everything), and the wired
  `vfs_conf.c` passes the syntax check with and without `-DHFS`.
- `var-write.sh` passes `sh -n`. Its superblock and HFSX parsing was tested on the real root image and on the volume.

None of this has been built by the kernel's real toolchain (`clang-24`, which knows `__builtin_xnu_type_*`; those
were stubbed for the host check). `.work/run-rebuild.sh` is the real test.

## 3. Builds still to do (offline, by whoever owns the tree)

**B0 [build] kernel**, in docker, after the touch/battery/WiFi work is done:

```sh
python3 darwin-lisa/kernel/drivers/ufs/ufs-wire.py          # if not already applied (after hakios-wire.py)
python3 darwin-lisa/kernel/drivers/ufs/datavol-wire.py      # driver patch + HFS Plus built in
darwin-lisa/.work/run-rebuild.sh
grep -n 'error:' research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/logs/kernel.log
grep -ac 'take writes as /dev/ufs_data' research/hackios-xnu-kernel/kernel26.nopac      # 1
grep -ac 'hfs_mountfs' research/hackios-xnu-kernel/kernel26.nopac                       # >0
```

**B1 [build] init26 and the root image.** The new init has to be inside the root image. That means rewriting
super's first 4608 MiB, which is the root, not the region:

```sh
darwin-lisa/userland/scripts/build-root-26.sh     # or: docker ... build-init.sh, then install out/launchd as
                                                  # userland-26/out/sysroot-26/sbin/launchd (mode 0755)
FULL=1 darwin-lisa/userland/scripts/mkimage-trim.sh          # -> userland-26/out/system-26.apfs.full
python3 -c "import struct;d=open('userland-26/out/system-26.apfs.full','rb').read(48);print(d[32:36],struct.unpack_from('<Q',d,40)[0])"
stat -c %s userland-26/out/system-26.apfs.full               # MUST be 4831838208 (1179648 blocks)
```

If the new image is larger than 4,831,838,208 B, because the content crossed a 32 MiB step, the region has to move.
Raise `DLISA_ROOT_BLOCKS` in `data-regions.h` and `ROOT_MIB`/`DATA_SEEK_MIB`/`DATA_MIB` in `var-write.sh`, then rebuild
the loader and the volume with `mkvarvol.py --blocks`. The loader and `var-write.sh` both refuse a root container that
reaches into the region, so a mistake here stops the write and does not corrupt anything.

## 4. Device sequence, option A

The pmOS password comes from `PMOS_PASS`, never from a file. `bootos.sh pmos` reaches pmOS.

**A0 [READ-ONLY] what is on the phone now:**

```sh
ssh mateus@172.16.42.1 'sudo head -c 364544 /dev/disk/by-partlabel/vendor_boot_a | sha256sum;
  cat /sys/class/block/sda20/start /sys/class/block/sda20/size; grep PARTNAME /sys/class/block/sda20/uevent;
  sudo blkid -p -o value -s PART_ENTRY_UUID /dev/sda20;
  sudo head -c 4831838208 /dev/disk/by-partlabel/super | sha256sum'
```

Expected:

- sda20 starts at sector 4587520 with 17825792 sectors, `PARTNAME=super`, and UUID
  `f1cc2373-2d04-a1a1-5cbf-e3d75f3a4a27`. If any of these differs, stop, fix `data-regions.h` and `var-write.sh`
  from the live values, and rebuild. Never widen a region.
- The root is `195ae6b5…` (`system-26.apfs.full`) or `7275eaf2…` (`-drivers`), whichever was written.

Save the current vendor_boot_a as the rollback, whatever is flashed:
`sudo dd if=/dev/disk/by-partlabel/vendor_boot_a of=/home/mateus/vendor_boot_a-before-datavol.img bs=1M`, then
`scp` it to `backups/`.

**A1 [RAM-ONLY] the new loader from RAM, before flashing.** Use the U-Boot menu's "HakiOS over fastboot", as in
flash-boot.md A2b, with `out/loader-usb-lisa-datavol.hkub`. Then boot a kernel with `ufs=2 ufs_write=1 ufs_data=super`
and nothing that mounts `/dev/ufs_data`. The loader log must show

```
ufs: super (LBA 573440..2801663 of unit 0): its blocks 1179648..2228223 (4096 MB) may be written this run, nothing else
```

Also try `ufs_data=userdata` and `ufs_data=nonexistent`. Each must print `no such region or partition: no write will
be served`. With the old kernel, `ufs_write=1` makes `/dev/ufs_super_data` over the same stretch, which is harmless
as long as nothing writes to it. Run this test without `var_ufs=1`.

**A2 [WRITES: vendor_boot_a] flash the loader** from pmOS:

```sh
scp out/vendor_boot-lisa-datavol.img mateus@172.16.42.1:/tmp/
ssh mateus@172.16.42.1 'sudo dd if=/tmp/vendor_boot-lisa-datavol.img of=/dev/disk/by-partlabel/vendor_boot_a bs=1M conv=fsync &&
  sudo sh -c "echo 3 > /proc/sys/vm/drop_caches" && sudo head -c 364544 /dev/disk/by-partlabel/vendor_boot_a | sha256sum'
# must be 7c1f3182f05015afd43be8a55b82fedb9f1fc63194aa02190d489991b380be44
```

Then run `tools/bootos.sh state` and `tools/bootos.sh pmos` (A5 in flash-boot.md) to confirm the OS switch still
works.

Rollback: the same `dd` with `backups/vendor_boot_a-before-datavol.img`. If pmOS is unreachable, use
`fastboot flash vendor_boot_a` from ABL (`tools/abl-fastboot.sh`).

**A3 [READ-ONLY] check the region and the image** on pmOS:

```sh
scp out/datavol/var-A.hfsx.gz darwin-lisa/tools/var-write.sh mateus@172.16.42.1:/home/mateus/
ssh mateus@172.16.42.1 'cd /home/mateus && sudo sh var-write.sh check var-A.hfsx.gz 63d252f16078e5f4ed6150e43e6bde666f1f4d281edfaa6be9b237de90ee9ea5'
```

`check` proves that `/dev/disk/by-partlabel/super` is sda20 by its start, size, name and unique GUID (with
util-linux `blkid`), that nothing holds or mounts it, and that the root container (NXSB) ends at or before block
1179648. It also decompresses the image to `/home/mateus/var-A.hfsx` (4 GiB on userdata) and checks its size,
sha256 and the HFSX signature.

Optional rollback copy of what is in the tail now (old Android leftovers, of no value):
`sudo sh var-write.sh backup /home/mateus/super-tail-before.gz`.

**A4 [WRITES: super bytes 4831838208..9126805503, the region only]:**

```sh
ssh mateus@172.16.42.1 'cd /home/mateus && sudo VAR_WRITE=yes sh var-write.sh write var-A.hfsx.gz 63d252f16078e5f4ed6150e43e6bde666f1f4d281edfaa6be9b237de90ee9ea5'
```

The step hashes the root (4608 MiB), then runs one `dd ... seek=4608 count=4096 bs=1M conv=notrunc,fsync` into the
partition node. It then drops the caches, reads the region back and compares its sha256, and hashes the root again,
which must not change. Expect about 1 to 2 minutes.

**A5 [WRITES: super bytes 0..4831838207, the root] only if B1 made a new root image** (the new init). Write it as
in flash-boot.md C2, either with `darwin-write.py` through ums or with the pmOS fallback, but limited to the root's
4608 MiB:

```sh
ssh ... 'sudo dd if=system-26.apfs.full of=/dev/disk/by-partlabel/super bs=1M count=4608 conv=notrunc,fsync &&
  sudo sh -c "echo 3 > /proc/sys/vm/drop_caches" && sudo head -c 4831838208 /dev/disk/by-partlabel/super | sha256sum'
ssh ... 'cd /home/mateus && sudo sh var-write.sh verify 63d252f16078e5f4ed6150e43e6bde666f1f4d281edfaa6be9b237de90ee9ea5'   # the region is untouched
```

A4 and A5 can run in either order. Each one leaves the other's range alone and checks it.

**A6 [WRITES: the region, through the kernel] first boot with the volume** (after B0 and A2):

```sh
XL_UFS_ARGS="rd=md0 ufs=2 ufs_part=super ufs_root=1 ufs_write=1 ufs_data=super" \
XL_EXTRA_ARGS="display=1 console_open=1 keystore=1 wlan_stub=1 init_run=/usr/libexec/usermanagerd:--init init_run_secs=60 cpufreq=1 cpufreq_max=2400 cpus=7 var_ufs=1" \
  darwin-lisa/tools/run-flashroot.sh
grep -anE 'ufs: super|/dev/ufs_data|VAR |hfs' research/hackios-xnu-kernel/logs/flashroot.log | head -30
```

Expected, in order:

1. The loader prints the A1 line.
2. The kernel prints `ufs: super: blocks 1179648..2228223 (4096 MB, LBA 1753088..2801663 of unit 0) take writes as
   /dev/ufs_data`, then `BSD root: ufs_super`.
3. init26 prints `VAR /private/var is HFS Plus from /dev/ufs_data (4096 MB), kept between boots`, then
   `VAR /private/var is the data volume on the flash: N entries made, 0 already there, 0 failed`, then
   `VAR sync every 30 s`.

Let the run go to the progress bar's end. That is the slow first boot, done once.

**A7 [RAM-ONLY] second boot**, with the same command. Expect `0 entries made, N already there` and a much shorter
time to SpringBoard. Compare the `flashroot-shot-*.png` timestamps with A6's.

**A8 [READ-ONLY] look at the volume from the PC** (optional). On pmOS, run
`sudo dd if=/dev/disk/by-partlabel/super bs=1M skip=4608 count=4096 | gzip -1 > var-now.gz`. On the PC, run
`gzip -d`, then `fsck.hfsplus -fy` on a copy, or `sudo mount -t hfsplus -o ro,loop` to inspect. Linux mounts a
journaled HFS+ read-only.

### If something goes wrong

| Symptom | What it means / do |
|---|---|
| loader: `no write will be served: ...` | the GPT or the root is not what `data-regions.h` says. The boot goes on with ramfs. Fix the header from the live GPT |
| kernel: `ufs_data=super: no write will be asked for: ...` | its reason is printed. ramfs |
| init: `HFS Plus from /dev/ufs_data: <errno>` | the volume is not HFSX, or HFS is not in the kernel. ramfs. Check A4's sha with `var-write.sh verify` |
| the volume is damaged | boot without `var_ufs=1` (ramfs), then rewrite it with A4. That empties it, and the first-boot work runs once more |
| the root image changed (A4/A5 say so) | rewrite the root (A5 / flash-boot.md C2) before booting iOS |

### Rollbacks

| What | How |
|---|---|
| the volume | `var-write.sh write` again (fresh, empty), or `var-write.sh backup`'s file |
| not using it | drop `var_ufs=1` (and `ufs_write=1 ufs_data=super`): ramfs as before. Nothing else changes |
| vendor_boot_a | `backups/vendor_boot_a-before-datavol.img` (A0), via pmOS `dd` or ABL `fastboot flash vendor_boot_a` |
| root | `userland-26/out/system-26.apfs.full` (flash-boot.md C2) |

### Status on the phone (2026-11-10)

Done, option A, all writes read back:

| What | Where | sha256 |
|---|---|---|
| loader with the data region | vendor_boot_a (later replaced by the standalone image `vendor_boot-lisa-standalone.img`, 53b2e254) | 7c1f3182 |
| kernel (datavol.patch + HFS Plus built in) | `kernel26.nopac` = `kernel-16k-datavol-hfs.bak` | 58ec7fde |
| empty HFSX volume | super bytes 4831838208.. | 63d252f1 (as written) |
| root with the new init | super bytes 0..4831838207 | 8b84a329, then the presentation roots 964f2343, 09af3c4c, d5383125 |

- A1 was replaced by a short negative run on the flashed loader (`ufs_data=nonexistent`): the loader prints
  `no such region or partition: no write will be served`, the kernel `no write will be asked for`, init falls back to
  ramfs. `bootos.sh uboot` did not bring up U-Boot's fastboot (the loader came back), so the RAM test of the loader was
  skipped.
- Boot 1 (host-run, `run-flashroot.sh` with `XL_FW_RAMDISK`): the loader and kernel lines of A6, then
  `hfs: Initializing the journal`, `hfs: mounted Data on device ufs_data`,
  `VAR /private/var is HFS Plus from /dev/ufs_data (4096 MB), kept between boots`, `178 entries made`. About 2.3 GB free,
  VBAT 4.1 V. No clonefile/ENOTSUP/F_PUNCHHOLE/snapshot/purgeable failures in the log.
- **The slow first boot is DataMigrator, not LaunchServices.** The bar is com.apple.datamigrator (it links
  ProgressUI). Its plugin host com.apple.migrationpluginwrapper aborts and launchd throttles it, and the migrator waits
  out each plugin's 20 minute watchdog (stackshot on the volume: `migration might be hung or deadlocked. Plugin:
  com.apple.locationd.migrator ... (20 minutes) ... (erase)`). It decides by `LastSystemVersion` in the
  `com.apple.migration` preferences of **mobile** (`/private/var/mobile/Library/Preferences/com.apple.migration.plist`,
  where it also keeps `DMUserDataDisposition`), plus the sentinel `/private/var/datamigrator/migrate`. A boot that is
  cut off never records the build, so every boot migrated again from 0% (boots 1 to 3: 81% at 11 min, 86% at 18 min).
- init26 now writes that record at every boot (`LastSystemVersion` = ProductBuildVersion, `DMLastMigrationResults`
  = {buildVersion, success}) for mobile, root and /private/var/preferences, and removes the sentinel and the migrator's
  context file. `var_migrate=1` leaves them alone. With it (boot 4) no progress bar is drawn at all.
- Standalone (autoboot) runs: the kernel log the host can fetch stops after about 5.8 KB, so the init and HFS lines are
  only seen in host-run boots. Host polls (`oem log`, screenshots) make the loader draw its fastboot text into the
  panel memory, which a static iOS screen does not repaint.

## 5. What to watch for on the first boots: APFS-only calls on /private/var

Today `/private/var` is ramfs, which also has none of APFS's features. HFS is therefore no worse than what already
boots, and it adds hard links, `exchangedata`, `F_FULLFSYNC`/`F_BARRIERFSYNC` (sent to the flash as a flush) and
`RENAME_EXCL`. Calls that keep failing (with `ENOTSUP`, `ENOTTY` or `EINVAL`), and who makes them:

| Call | HFS here | Likely callers on iOS 26 | Expect |
|---|---|---|---|
| `clonefile`/`fclonefileat`/`copyfile(COPYFILE_CLONE)` | ENOTSUP (no VNOP_CLONEFILE) | installd/MobileInstallation (staging apps), containermanagerd (container templates), lsd, cfprefsd safe-save, backupd, assetsd | `COPYFILE_CLONE` falls back to a copy, so the step is slower. `COPYFILE_CLONE_FORCE` and plain `clonefile` callers log and may fail one operation |
| APFS snapshots (`fs_snapshot_*`) | ENOTSUP | softwareupdateservicesd/UpdateBrainService (OTA), backupd, mobile_obliterator | not on the boot path; logs only |
| purgeable / space fsctls (`APFSIOC_*_PURGEABLE*`, `ATTR_CMNEXT_PRIVATESIZE`) | ENOTTY / 0 | deleted / CacheDelete, mediaanalysisd, nsurlsessiond, Settings storage | logs; "purgeable space" reads 0 |
| `F_PUNCHHOLE` | not supported | logd (Persist/Special tracev3 files), corespotlightd | logd falls back to truncation. Watch for `logd` errors about `F_PUNCHHOLE` |
| data protection: `F_SETPROTECTIONCLASS`, `open_dprotected_np`, `ATTR_CMN_DATA_PROTECT_FLAGS` | ENOTSUP (`CONFIG_PROTECT 0` in HFS, no AppleKeyStore) | securityd/keychain sqlite, every NSFileProtection user, keybagd | same as ramfs today |
| `renamex_np(RENAME_SWAP)` | **was** ENOTSUP (`RENAME_EXCL` works); since `kernel/patches/hfs-rename-swap.patch` (2026-11-11) three renames under the VFS rename lock | Foundation's `replaceItemAtURL` and `_NSIRD` staging (lsd), usermanagerd's persona manifest and user session, containermanagerd's metadata | it did NOT fall back: usermanagerd never saved `/private/var/keybags/persona.kb` ("Failed to atomically swap writing file: [45]"), CoreServices' persona list was ENOENT, and MobileInstallation registered no app that needs a data container (25 of 252 on the home screen). Watch `Operation not supported` with `swap` in any daemon's log |
| `UF_DATAVAULT` / `SF_DATALESS` flags | ignored or EINVAL | TCC/privacy directories, FileProvider | watch `chflags` errors in tccd/containermanagerd |
| volume role / fstype checks (`statfs` `f_fstypename == "apfs"`, `APFSIOC_GET_VOLUME_ROLE`) | `hfs`, no role | MobileStorage, containermanagerd, lsd | today they see `ramfs` |
| sparse files | none (HFS allocates) | sqlite WAL/shm, dyld caches in /var | space only; 4 GiB budget |

**HakiOS 16.5 shims.** None for any of these. Nothing in its `userland/src` (inject\*, tweaks) works around
clonefile, snapshots, purgeable space or `F_PUNCHHOLE`. Its only `/private/var` handling was the HFS mount, the
skeleton and the logd markers, and its fixes were in the kernel (ramfs page-cache coherency, `cow_test.c`). On the
first A6/A7 boots, grep the log for `clonefile`, `ENOTSUP`, `Operation not supported`, `F_PUNCHHOLE`, `snapshot`
and `purgeable`, and note which daemon prints them.

Space: 4 GiB holds the first-boot state of 252 apps (the LaunchServices database, containers, caches) with room
to spare, but app installs will not fit. That is B's job.

## 6. Risks (option A)

- **The kernel's real compile** may still fail on something the host check could not see, such as `clang-24`'s
  kalloc_type checks inside HFS. HakiOS built the same sources with Apple's clang for xnu-8796.
- **HFS on this kernel has never run.** The journal replay and `DKIOCSYNCHRONIZE`-to-`BLOCK_OP_FLUSH` path is the
  same as HakiOS's. Worst case, a damaged volume, which A4 rewrites.
- **A reset in the middle of a run** loses at most `var_sync` seconds of data. The journal keeps the metadata
  consistent. No orderly unmount happens on this port: `loader_return` is not reached on 16 KB pages, and a run ends
  in `fastboot reboot`.
- **A root rewrite (A5) that overflows** into the region is impossible with `count=4608`, and both checks would
  catch it. A larger root image means moving the region (B1).
- **`ufs_native=1`.** The list switching after writes is new code. It prints a `ufs: native:` line each time it
  switches. If those lines look wrong, use `ufs_data_restore_ms=0` (the old behavior) or leave `ufs_native` off,
  which is the flash-root default today.

## 7. Option B (later): its own partition, and migrating A to B

B is a partition of its own, for example `darwin_var` of 16 to 32 GiB, carved from the end of userdata. Only
`data-regions.h` (a second line) and `ufs_data=darwin_var` change. The kernel, init and tools stay the same. Use
type GUID `48465300-0000-11AA-AA11-00306543ECAC` (Apple HFS+).

### The no-keys shrink of pmOS

ext4 cannot shrink while mounted, and pmOS's root is ext4 p2 of the image on userdata. Two ways that need no
buttons:

1. **pmOS's own initramfs debug shell (preferred today).** pmOS boots through U-Boot's
   `sysboot ... /extlinux/extlinux.conf` on its boot subpartition, so the kernel command line can be edited from
   running pmOS. Adding `pmos.debug-shell` to the `append` line stops the initramfs-extra stage before the root is
   mounted (`init_2nd.sh`), with telnet on `172.16.42.1:23`. initramfs-extra ships `e2fsck`, `resize2fs`, `parted`,
   `losetup` and `blkid`. Steps:
   - **B-1 [WRITES: pmOS /boot/extlinux/extlinux.conf]** First do a dry run: add `pmos.debug-shell`, reboot into
     pmOS, telnet in, then continue the boot (`pmos_continue_boot`). Remove the option afterwards. Nothing else
     happens until this round trip works.
   - **B-2 [WRITES: userdata inner ext4]** In the debug shell, run `e2fsck -f /dev/<loop>p2`, then
     `resize2fs /dev/<loop>p2 <new size - 64M>`.
   - **B-3 [WRITES: userdata inner GPT]** Shrink p2: `parted ---pretend-input-tty /dev/<loop> resizepart 2 <end>`,
     confirming with Yes. Then `resize2fs` again with no size (to fit exactly), and `e2fsck -f`.
   - **B-4 [WRITES: sda GPT]** From normal pmOS, shrink entry 33 (userdata) and add entry 34 (`darwin_var`) with
     `sgdisk`, as in flash-boot.md branch 3, step 5. Keep the type, unique GUID and name of 33. The inner image's
     backup GPT now lies past userdata's new end. Linux and U-Boot use the primary header with a warning. `sgdisk -e`
     on the loop device rewrites the backup at the new end.

   Risk: medium. If telnet to the debug shell never comes up, pmOS stays in initramfs. Recovery without keys is then
   only the loader (still on a cold boot: menu, 5 s, HakiOS), `oem bootos:ums` once U-Boot's ums works, or ABL's
   `fastboot flash userdata` with `out/pmos-lisa-r17-20261110/`, which erases pmOS's state. Hence the B-1 dry run,
   and an `e2image -ra` of p2 to the PC before B-2 (flash-boot.md branch 3 step 2). pmOS's
   `resize_root_partition` (`resizepart 2 100%` on every boot) does nothing once p2 fills the smaller image.
2. **From the PC over ums**, once the other worker's U-Boot ums works. This is flash-boot.md branch 3 as written,
   with everything offline and pmOS not running. It is the lowest risk; only the waiting is the cost.

Not worth it: a RAM-only rootfs for pmOS. The debug shell is that, already built in.

### Migrating A to B (copy, then grow)

1. **[WRITES: sda GPT, userdata]** Create `darwin_var` (above). Add its exact line to `data-regions.h`
   (`write_first = 0`, `write_end` = its size in blocks, `root_below = 0`). Rebuild with `build-ossel.sh datavol` and
   flash vendor_boot_a (A2).
2. **[WRITES: darwin_var's first 4 GiB]** Copy the volume block for block from pmOS:
   `dd if=/dev/disk/by-partlabel/super of=/dev/disk/by-partlabel/darwin_var bs=1M skip=4608 count=4096 conv=notrunc,fsync`.
   Compare `sha256sum` of both 4096 MiB ranges after dropping caches. Make `var-write.sh` a copy with B's constants
   for the checks: start, size, name, UUID.
3. **[WRITES: darwin_var, through the kernel]** Boot once with `ufs_write=1 ufs_data=darwin_var var_ufs=1 var_grow=1`.
   HFS mounts the 4 GiB volume, which is smaller than the device. init26 then asks HFS to resize to the whole
   device (`HFSIOC_RESIZE_VOLUME`, `hfs_extendfs`) and prints `VAR the data volume is now N MB`. Later boots
   leave `var_grow` out.
4. Keep A's copy in super's tail until B has run for a while. Going back means booting with `ufs_data=super`.

Alternative without the resize: `mkvarvol.py --blocks <B's size>` makes an empty B volume, and the first-boot work
runs once more. That is simpler and loses the state.

### Status (2026-11-12): B is the layout on the phone

The carve, with the user's authorization (pmOS's staging copies of old root images in `/home/mateus`, 72 GB, were
deleted first; pmOS's root then used 6.8 GB). Blocks of 4096 on LUN 0, everything 1 MiB aligned:

| GPT entry | Before | After |
|---|---|---|
| 33 `userdata` (type `1b81e7e6-…`, unique `76152dc1-…`, both kept) | LBA 2967552..30617594 (105.48 GiB) | LBA 2967552..22228735, 19261184 blocks (73.47 GiB) |
| 34 `darwin_var` (type Apple HFS+ `48465300-0000-11aa-aa11-00306543ecac`, unique `7b9f2109-07d5-4cb6-941b-d974a2425d30`) | none | LBA 22228736..30617343, 8388608 blocks (exactly 32 GiB) |
| unallocated | none | LBA 30617344..30617594 (251 blocks, 1 MiB) |

Inside userdata, pmOS's image (4096-byte sectors, 128-entry GPT): p1 `pmOS_boot` 2048..124927 unchanged; p2
`pmOS_root` from 124928..27650037 (27525110 blocks) to 124928..19261178 (19136251 blocks, 73.0 GiB), the ext4 resized
to exactly that, and the inner backup GPT moved to blocks 19261179..19261183 (`sgdisk -e` on a loop device limited to
the new size, BEFORE the outer shrink: Linux's and U-Boot's GPT parsers reject a primary header whose `last_usable_lba`
lies past the device, so the inner table has to be consistent with the smaller userdata the moment it exists).

How it was done, no keys, the phone reached only over USB:

1. **Backups** in `backups/lisa-planb-20261112/`: sda's GPT head and tail (64 blocks each, equal to the 2026-11-10
   backup), userdata's inner head and tail, `dumpe2fs -h`, `extlinux.conf`, and the HFSX data volume from super's tail
   (`var-super-tail.hfsx.zst`, raw sha256 `e12cd4a6…`, verified after decompression). No image of pmOS itself: the user
   said the restore set `out/pmos-lisa-r17-20261110/` is enough.
2. **The debug-shell round trip** (`tools/pmos-debug-shell.py`: telnet to 172.16.42.1:23, one marker per command):
   `pmos.debug-shell` appended to the `append` line of `/boot/extlinux/extlinux.conf`, `bootos.sh hakios`, `fastboot oem
   bootos:pmos`, the shell answers in about 15 s, `pmos_continue_boot` boots pmOS. The initramfs (3.12.5) enters the shell
   BEFORE `mount_subpartitions`, so the loop device is set up by hand there (`losetup --show -Pf --direct-io=on
   --sector-size 4096 /dev/sda33`) and detached before continuing. It has `e2fsck`, `resize2fs`, `parted`, `losetup`,
   `blkid`, `fdisk`; no `sgdisk`.
3. **Everything else in that ONE shell session** (`tools/planb-session.sh`, staged on pmOS by SSH, fetched from the
   read-only mounted root, run only when its sha256 matches). The first attempt did the ext4 shrink alone and
   continued the boot: the initramfs's `resize_root_partition` + `resize_root_filesystem` promptly grew p2 and the
   filesystem back to fill userdata. So the shrink, the inner table and the outer table all happen before the boot
   goes on, 18 s in all:
   - `e2fsck -f -y`, `resize2fs /dev/loop0p2 19136251`, `e2fsck -f -y` (2,398,167 blocks used; minimum 2,247,314);
   - `sgdisk` and its libraries copied out of the pmOS root into the initramfs (`/tmp/sg`, run through
     `ld-musl-aarch64.so.1 --library-path`): the initramfs has none;
   - the inner GPT through a second loop device limited to the new size (`losetup --sector-size 4096 --sizelimit
     $((19261184*4096)) /dev/sda33`): `sgdisk -e -d 2 -n 2:124928:0 -t 2:B921B045-… -u 2:467BD7D3-… -c 2:primary`,
     verified by the kernel's own parse of a `-P` loop at that size (p2 = 153090008 sectors of 512) and `e2fsck -fn`;
   - the outer GPT: `sgdisk -d 33 -n 33:2967552:22228735 -t 33:1b81e7e6-… -u 33:76152dc1-… -c 33:userdata -n
     34:22228736:30617343 -t 34:48465300-… -u 34:7b9f2109-… -c 34:darwin_var /dev/sda`, then the ORIGINAL block 0
     put back (sgdisk rewrites the protective MBR's CHS and size fields; Qualcomm's has size 0xFFFFFFFF), then the
     head and tail read back (`iflag=direct`) and compared with the rehearsal's hashes (`d24813db…`, `554b6ad3…`),
     `blockdev --rereadpt` (sda33 +154089472, sda34 +67108864, `PARTNAME=darwin_var`).
   The rehearsal: the same sgdisk command on a sparse file carrying the live head and tail, from normal pmOS
   (`sgdisk -v` clean, the kernel parses it through `losetup -P`), compared field by field on the PC: only entries 33
   and 34 and the two CRCs change; the header's other fields, the disk GUID and the unused blocks stay.
4. `pmos_continue_boot`: pmOS boots from the smaller userdata (U-Boot's `pmos_boot` takes `part start/size` of
   userdata from the new GPT; the initramfs finds nothing to grow). `pmos.debug-shell` removed again.

Then the wiring of section "Migrating A to B": `loader/data-regions.h` names only `darwin_var` now (A's super line is
retired: `ufs_data=super` answers "no such region"), `build-standalone.sh` takes `DATAVOL_PART` (default `darwin_var`),
`tools/var-write.sh`, `flash-root.sh` and `metal-cache-seed.sh` address the partition, and the HFSX volume was copied
block for block from super's tail into `darwin_var` and grown there with `var_grow=1`. super is the root's alone:
`mkimage-trim.sh` bounds the FULL image by super's 9,126,805,504 bytes (padded to 32 MiB, no longer to a fixed size), and
`flash-root.sh` writes the image's own size.
