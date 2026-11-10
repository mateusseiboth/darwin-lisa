# UFS flash: HakiOS's block driver on xnu-12377, and the root on a GPT partition

HakiOS's `hakios.ufs.c` / `hakios.ufshc.c` / `hakios.ufshc.h` (xnu-8796), vendored and rebased onto the iOS 26
kernel as the optional tag `hakios_ufs`. Every GPT partition of the phone becomes `/dev/ufs_NAME` (block) and
`/dev/rufs_NAME` (character), read only. Reads go through the loader's mailbox page (the loader stays on core 0
and calls UEFI's UFS driver) or through the kernel's own UFS host controller driver (`ufs_native=1`).

Changes against HakiOS: no iOS 16 layout (`UFS_WRITE_FIRST` / `UFS_DARWIN_MIN` are gone; the port's partition is
named by a boot-arg and bounded by its GPT entry), `hakios_ufs_root()` for IOFindBSDRoot, `hakios_ufs_init()` runs
once, the strategy routine maps cluster buffers (`buf_map`), `ufs_loader=0`, and `<vm/vm_kern_xnu.h>` in ufshc.c.

## Boot-args

All of them go in the payload command line (`mkpayload.py --cmdline`): the loader reads the same line.

| boot-arg | what |
|---|---|
| `ufs=2` | the loader publishes its page (`ufs_shared_pa=` is added for the kernel). Without it the driver is inert. |
| `ufs_part=NAME` | the port's partition, by GPT name: `super` (sda20, LUN 0), or `darwin`. Without it nothing is the root and nothing writes. |
| `ufs_root=1` | the root is `/dev/ufs_NAME`. **Keep `rd=md0`** on the line (see below). If the partition is not there, the root falls back to `rd=`. |
| `ufs_native=1` | reads by the kernel's own UFSHC driver (31 requests in flight instead of one); falls back to the mailbox on an error. `ufs_native=2` only looks, `3` is a dry run. |
| `ufs_loader=0` | the loader will not answer (it lost core 0). Nothing is asked through the mailbox; requires `ufs_native=1`; LUN 0 is identified by its own GPT header (crc, backup header at the unit's last block). Writes are refused. |
| `ufs_write=1` | writes into `/dev/rufs_NAME` only (and `/dev/ufs_NAME_data`), whole blocks, from the floor to the end of the partition. Off by default. The loader must offer the same partition for writes. **With `datavol.patch`:** only `/dev/ufs_data`, with `ufs_data=NAME` (below) |
| `ufs_data=NAME` | (datavol.patch) the partition the loader's write region is in: `super` for option A. See `docs/data-volume.md` |
| `ufs_write_first=N` | the floor, in 4096-byte blocks. The kernel uses max(N, the loader's `write_first`). 0 means no writes at all. Put it past the end of the APFS container (`nx_block_count`). |
| `apfs_root_c=` / `apfs_root_v=` | which container / volume of the root device (APFS port, unchanged). |
| `apfs_cheap=0/1` | overrides the request-cost hint; with this wire the flash is "worth putting together" by default. |

Test-only, darwin-lisa: `ufs_probe=1` (read only, root unchanged): in its own thread, `ufs_probe_delay=` s after the
storage is up (default 20), the loader's partition table, the LP geometry / header words of the `ufs_part=` partition
(at 4096 and 12288, what `tools/lpdump.py` reads), and `ufs_probe_mb=` MB of it (default 256) read in 1 MB requests,
timed (`ufs: probe:` lines).

Test-only, from HakiOS: `ufs_check=1/2`, `ufs_bench=1`, `ufs_native_check=1`, `ufs_native_bench=1`. They call
`loader_return()` to end the run, but on the 16 KB kernel `loader_return()` does not jump back (loader_console.c is
4 KB only), so the boot simply goes on after them.

### Why `rd=md0` stays

* `bsd_init` checks the root's seal (`FSIOC_KERNEL_ROOTAUTH`) unless `rd=md0`. The APFS port answers `ENOTTY`, which
  panics with `rootvp not authenticated after mounting`.
* launchd reads `rd=md0` and runs as a Restore environment. init26 and the whole userland are built on that.

So the flash root is chosen by `ufs_root=1`, which IOFindBSDRoot checks before it reads `rd=`. Without an uploaded
RAM disk the loader does not add `rd=md0`, so put it in the payload line yourself. md0 is never opened then.

## Root mount: chosen design

`rd=md0 ufs=2 ufs_part=super ufs_root=1` (+ `ufs_native=1`): IOFindBSDRoot calls `hakios_ufs_root()`, which brings
the storage up (before devfs is mounted, as memdev does) and returns `makedev(ufs bmajor, index)`. `vfs_mountroot`
opens it FREAD, `spec_open` switches the block size to 512 and back (the driver accepts 512..4096), and
`apfs_mountroot` reads with `DKIOCGETBLOCKSIZE` = 4096 (`APFS_META_CHUNK % 4096 == 0`) and `DKIOCGETBLOCKCOUNT`.
The bdev refuses writes, the APFS mount is `MNT_RDONLY`.

The alternative, HakiOS's pivot (md0 boot volume, init mounts `/dev/ufs_NAME` and calls `kern.hakios_switch_root`),
needs a sysctl around `vfs_switch_root()` that this tree does not have (the function exists, nothing calls it
from user space), init26 changes, and still an md0 upload. The direct root
is about 15 lines in IOKit and needs no userland change.

## Wire and rebuild

```
docker run --rm -v ~/Dev/mobile:/home/mateusseiboth/Dev/mobile darwin-lisa/unixnu-toolchain:latest \
  python3 /home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/drivers/ufs/ufs-wire.py
darwin-lisa/.work/run-rebuild.sh
grep -n 'error:' research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/logs/kernel.log
grep -ac 'ufs_root=1' research/hackios-xnu-kernel/kernel26.nopac      # the new strings are in the kernel
```

Run it after `drivers/hakios-wire.py`: that script re-copies `hakios.stubs.c` and `hakios.input.c` from HakiOS.
`run-rebuild.sh` does not stop on a compile error and keeps the old `kernel.macho`, so always grep the log.
The script edits: `bsd/conf/files`, `config/MASTER`, `config/MASTER.arm64.bcm2837`,
`iokit/bsddev/IOKitBSDInit.cpp` (declaration + hook), `bsd/miscfs/apfs/apfs_vfsops.c` (the stub `hakios_ufs_dev`
becomes the driver's).

## Read-only test sequence

Nothing writes in steps 1 to 4: no `ufs_write=1` anywhere.

1. **Table**: `ufs=2` only, root from md0 as today. Expect `ufs: N partitions ... /dev/ufs_NAME`; on the phone,
   `ls -l /dev/ufs_super /dev/rufs_super`.
2. **Reads**: `dd if=/dev/rufs_super bs=1m count=8 | shasum` and compare with the same 8 MB read on the host
   from the image that was flashed. `sysctl kern.hakios.ufs_requests kern.hakios.ufs_errors`.
3. **Native path**: `ufs=2 ufs_native=1`. Expect `proof: TEST UNIT READY ... came back here` and
   `unit0 is LUN 0: its GPT header read both ways is the same`. Repeat the hash of step 2.
4. **Root**: `rd=md0 ufs=2 ufs_native=1 ufs_part=super ufs_root=1`. Expect `BSD root: ufs_super, major M, minor I`,
   then `apfs: device M,I: requests to it are worth putting together` and `apfs: volume 0 "System"`.
   A RAM disk is not needed; `rd=md0` must still be on the line.
5. **Writes**: the data volume, below.

## Data volume: /dev/ufs_data (not applied)

`datavol.patch` + `datavol-wire.py` (run after `ufs-wire.py`) + `hfs_compat_12377.h`; the run book is
`docs/data-volume.md`. `ufs_write=1 ufs_data=NAME` makes `/dev/ufs_data`, a read-write block device over exactly the
region the loader offers (`loader/data-regions.h`: GPT name, exact LBAs, `[write_first, write_end)`). For option A,
that region is super's tail past the root image. The wire script also builds HFS Plus into the kernel (HakiOS's
`bsd/hfs`, Apple's hfs-627.100.6), because the port's APFS is read-only. Character devices no longer write, and
`ufs_write_first=` can only raise the floor. With the patch, `ufs_write=1` without `ufs_data=` writes nothing.
