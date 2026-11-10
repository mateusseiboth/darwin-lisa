/*
 * darwin-lisa: the regions of the phone's flash the loader may write for the kernel (data-region.patch).
 *
 * A write is served only inside ONE region, the one the run names with "ufs_write=1 ufs_data=NAME", and only if
 * the GPT partition of that name is exactly what its line says: type GUID, unique GUID, first and last LBA on
 * logical unit 0 (blocks of 4096), and nothing else of that name. Everything else answers EFI_ACCESS_DENIED.
 *
 * The values come from the live GPT (docs/data-volume.md, step A0/A1). If the phone's GPT differs, the loader
 * prints the partition it found and writes nothing: fix this file from the live GPT and rebuild
 * (loader/build-ossel.sh datavol). Never widen a region to make a mismatch go away.
 *
 * GUIDs are written as EFI_GUID: { a, b, c, { d0..d7 } } for the text form aaaaaaaa-bbbb-cccc-d0d1-d2d3d4d5d6d7.
 */
#ifndef DARWIN_LISA_DATA_REGIONS_H
#define DARWIN_LISA_DATA_REGIONS_H

/*
 * Option A: the tail of super after the iOS root image.
 *   super = GPT entry 20 of sda (LUN 0), LBA 573440..2801663, 2228224 blocks (9,126,805,504 B)
 *     type   89a12de1-5e41-4cb3-8b4c-b1441eb5da38 (Android super)
 *     unique f1cc2373-2d04-a1a1-5cbf-e3d75f3a4a27      (backups/lisa-pmos-20261108/gpt-sda-head.bin)
 *   root   = userland-26/out/system-26.apfs.full at block 0, 4,831,838,208 B = 1179648 blocks (padded to 32 MiB;
 *            its container says nx_block_count 1176510). root_below makes the loader read that superblock at
 *            every run and refuse if the container ever reaches past write_first.
 *   data   = blocks [1179648, 2228224) of super = 1048576 blocks = exactly 4 GiB (4,294,967,296 B),
 *            byte offset 4,831,838,208 of super; absolute LBA 1753088..2801663 on LUN 0.
 */
#define DLISA_SUPER_FIRST_LBA   573440ULL
#define DLISA_SUPER_LAST_LBA    2801663ULL
#define DLISA_ROOT_BLOCKS       1179648ULL      /* the root image's bytes / 4096: the floor of the data region */
#define DLISA_SUPER_BLOCKS      (DLISA_SUPER_LAST_LBA - DLISA_SUPER_FIRST_LBA + 1)

_Static_assert(DLISA_SUPER_BLOCKS == 2228224ULL, "super is 2228224 blocks of 4096 (9,126,805,504 B)");
_Static_assert(DLISA_ROOT_BLOCKS * 4096ULL == 4831838208ULL, "the floor is the root image's end");
_Static_assert(DLISA_SUPER_BLOCKS - DLISA_ROOT_BLOCKS == 1048576ULL, "the data region is 4 GiB");

static const struct block_region block_regions[] = {
	{
		.name = "super",
		.type = { 0x89a12de1, 0x5e41, 0x4cb3, { 0x8b, 0x4c, 0xb1, 0x44, 0x1e, 0xb5, 0xda, 0x38 } },
		.unique = { 0xf1cc2373, 0x2d04, 0xa1a1, { 0x5c, 0xbf, 0xe3, 0xd7, 0x5f, 0x3a, 0x4a, 0x27 } },
		.first_lba = DLISA_SUPER_FIRST_LBA,
		.last_lba = DLISA_SUPER_LAST_LBA,
		.write_first = DLISA_ROOT_BLOCKS,
		.write_end = DLISA_SUPER_BLOCKS,
		.root_below = 1,
	},
	/*
	 * Option B (later, docs/data-volume.md "Migrating A -> B"): a partition of its own carved from userdata, the
	 * whole of it writable. Fill in from the live GPT after the carve, then rebuild. Example shape:
	 *
	 * {
	 *	.name = "darwin_var",
	 *	.type = { 0x48465300, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } },  (Apple HFS+)
	 *	.unique = { ... from sgdisk -i 34 ... },
	 *	.first_lba = <start>, .last_lba = <end>,
	 *	.write_first = 0, .write_end = <end - start + 1>,
	 *	.root_below = 0,
	 * },
	 */
};

#endif
