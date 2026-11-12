/*
 * darwin-lisa: the regions of the phone's flash the loader may write for the kernel (data-region.patch).
 *
 * A write is served only inside ONE region, the one the run names with "ufs_write=1 ufs_data=NAME", and only if
 * the GPT partition of that name is exactly what its line says: type GUID, unique GUID, first and last LBA on
 * logical unit 0 (blocks of 4096), and nothing else of that name. Everything else answers EFI_ACCESS_DENIED.
 *
 * The values come from the live GPT (docs/data-volume.md, section 7). If the phone's GPT differs, the loader
 * prints the partition it found and writes nothing: fix this file from the live GPT and rebuild
 * (loader/build-combined.sh). Never widen a region to make a mismatch go away.
 *
 * GUIDs are written as EFI_GUID: { a, b, c, { d0..d7 } } for the text form aaaaaaaa-bbbb-cccc-d0d1-d2d3d4d5d6d7.
 */
#ifndef DARWIN_LISA_DATA_REGIONS_H
#define DARWIN_LISA_DATA_REGIONS_H

/*
 * Option B (2026-11-12): the data volume has a partition of its own, carved from the end of userdata, the whole of
 * it writable. super (GPT entry 20, LBA 573440..2801663) holds only the iOS root now, which may use all of its
 * 2228224 blocks: the loader never writes it, and no region names it any more.
 *   darwin_var = GPT entry 34 of sda (LUN 0), LBA 22228736..30617343, 8388608 blocks = exactly 32 GiB
 *     type   48465300-0000-11aa-aa11-00306543ecac (Apple HFS+)
 *     unique 7b9f2109-07d5-4cb6-941b-d974a2425d30 (chosen at the carve, backups/lisa-planb-20261112/)
 *   userdata (entry 33) ends at LBA 22228735 since the carve; the 251 blocks after darwin_var, up to the last
 *   usable LBA 30617594, are unallocated.
 *
 * Option A (super's tail after a 4,831,838,208-byte root, 2026-11-10 to 2026-11-12) is retired: a loader built from
 * this file answers "no such region" to ufs_data=super, and init26 falls back to ramfs.
 */
#define DLISA_VAR_FIRST_LBA     22228736ULL
#define DLISA_VAR_LAST_LBA      30617343ULL
#define DLISA_VAR_BLOCKS        (DLISA_VAR_LAST_LBA - DLISA_VAR_FIRST_LBA + 1)

_Static_assert(DLISA_VAR_BLOCKS == 8388608ULL, "darwin_var is 8388608 blocks of 4096 (32 GiB)");
_Static_assert(DLISA_VAR_FIRST_LBA % 256 == 0, "darwin_var starts on a 1 MiB boundary");

static const struct block_region block_regions[] = {
	{
		.name = "darwin_var",
		.type = { 0x48465300, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } },
		.unique = { 0x7b9f2109, 0x07d5, 0x4cb6, { 0x94, 0x1b, 0xd9, 0x74, 0xa2, 0x42, 0x5d, 0x30 } },
		.first_lba = DLISA_VAR_FIRST_LBA,
		.last_lba = DLISA_VAR_LAST_LBA,
		.write_first = 0,
		.write_end = DLISA_VAR_BLOCKS,
		.root_below = 0,
	},
};

#endif
