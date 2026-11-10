/*
 * A file of one of this port's RAM disks, by path, for the drivers that want one before there is
 * a root or without asking it: the GPU's firmware (hakios.adreno.c), the Wi-Fi DSP's
 * (hakios.wifi.c), the audio DSP's (hakios.adsp.c) -- "fw/NAME" of the image that travels with
 * the kernel (md1; md0 when there is nothing else) or of the resident one (md0).
 *
 * The image is an APFS container (tools/apfs/mkapfs.py: what userland/build.sh makes).  A file
 * stored as it is in one piece is used where it lies in the RAM disk's memory, as the drivers
 * always did (build.sh has mkapfs.py store fw/ that way: --plain fw).
 * Any other APFS file is read out once by apfs_lib -- the same code as the volume's when
 * mounted, compressed or not -- into memory of its own, and kept: firmware is asked for once
 * or twice a session and its driver keeps it while the phone runs, so nothing gives it back.
 *
 * darwin-lisa: when no RAM disk has the file and the root is the flash's own (ufs_root=1
 * ufs_part=NAME, kernel/drivers/ufs), fw/NAME of the root itself, read through the UFS driver
 * (the root image is the APFS container at the start of that partition). A boot from the flash
 * alone has no RAM disk, and the root image carries fw/ (userland-26/out/sysroot-26/fw). Always a
 * copy; *disk_out is 2 then. Boot-arg image_ufs=0 leaves the root out.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <miscfs/apfs/apfs_lib.h>
#include <pexpert/pexpert.h>

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
#ifdef HAKIOS_UFS
extern int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes);
#endif

#define UFS_ROOT_DISK   2               /* what *disk_out says for the flash's root (md0 and md1 are 0 and 1) */

/* ---- APFS ---- */

struct mem_dev {
	const uint8_t *base;
	uint64_t       length;
};

static int
mem_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct mem_dev *m = ctx;

	if (off > m->length || len > m->length - off) {
		return EIO;
	}
	memcpy(buf, m->base + off, len);
	return 0;
}

static LCK_GRP_DECLARE(image_lck_grp, "hakios_image");
static LCK_MTX_DECLARE(image_lock, &image_lck_grp);    /* kept[]: the drivers start beside each other */
static struct {
	int       disk;
	char      path[96];
	uint8_t  *data;
	uint64_t  size;
} kept[64];

/*
 * fw/NAME of the container SOURCE reads. BASE: where it lies in memory (a RAM disk), so that a file in one
 * piece is used in place; NULL (the flash): always a copy.
 */
static const uint8_t *
apfs_file(int disk, const struct apfs_dev *source, const uint8_t *base, const char *dir, const char *name, uint64_t *size)
{
	struct apfs_dev dev = *source;
	uint64_t length = source->size;
	uint8_t magic[4];
	struct apfs_nx *nx = NULL;
	struct apfs_vol *vol = NULL;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	char path[96];
	uint64_t id = 0, fsize = 0, off = 0;
	uint8_t dt = 0, *data = NULL;
	uint32_t i;
	int error;

	if (length < 4096 || dev.read(dev.ctx, 32, magic, sizeof(magic)) != 0 || memcmp(magic, "NXSB", 4) != 0) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/%s/%s", dir, name);
	lck_mtx_lock(&image_lock);
	for (i = 0; i < sizeof(kept) / sizeof(kept[0]) && kept[i].data != NULL; i++) {
		if (kept[i].disk == disk && strcmp(kept[i].path, path) == 0) {
			*size = kept[i].size;
			lck_mtx_unlock(&image_lock);
			return kept[i].data;
		}
	}
	nx = kalloc_type(struct apfs_nx, Z_WAITOK | Z_ZERO);
	vol = kalloc_type(struct apfs_vol, Z_WAITOK | Z_ZERO);
	if (nx == NULL || vol == NULL) {
		goto out;
	}
	if (apfs_nx_open(nx, &dev) != 0) {
		kfree_type(struct apfs_nx, nx);
		nx = NULL;
		goto out;
	}
	error = ENOENT;
	for (uint32_t v = 0; v < APFS_NX_MAX_FILE_SYSTEMS && error != 0; v++) {
		if (nx->fs_oid[v] != 0 && apfs_vol_open(nx, v, vol) == 0) {
			error = apfs_path_lookup(vol, path, &id, &dt);
		}
	}
	if (error == 0) {
		error = apfs_inode_get(vol, id, &ino);
	}
	if (error == 0 && (ino.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		error = EISDIR;
	}
	if (error == 0) {
		error = apfs_inode_size(vol, &ino, &fsize);
	}
	if (error == 0 && (fsize == 0 || fsize > 256 * 1024 * 1024)) {
		error = EFBIG;
	}
	if (error == 0) {
		uint64_t addr = 0;

		/* in one piece as it is: where it lies, no copy */
		if (base != NULL && apfs_file_in_place(vol, &ino, &addr) == 0 && addr <= length / nx->block_size &&
		    fsize <= length - addr * nx->block_size) {
			apfs_nx_close(nx);
			*size = fsize;
			data = __DECONST(uint8_t *, base) + addr * nx->block_size;
			goto out;
		}
		if (i == sizeof(kept) / sizeof(kept[0])) {
			printf("hakios_image: %s of /dev/md%d: kept %lu files already, no room for a copy of another\n", path, disk,
			    (unsigned long)i);
			error = ENOSPC;
		}
	}
	if (error == 0) {
		data = kalloc_data((vm_size_t)fsize, Z_WAITOK);
		error = data == NULL ? ENOMEM : 0;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	while (error == 0 && off < fsize) {
		size_t got = 0;

		error = apfs_read_file(vol, &ino, &cf, off, data + off, (size_t)(fsize - off), &got);
		if (error == 0 && got == 0) {
			error = EIO;
		}
		off += got;
	}
	apfs_cfile_fini(&cf);
	apfs_nx_close(nx);
	if (error == 0) {
		kept[i].disk = disk;
		strlcpy(kept[i].path, path, sizeof(kept[i].path));
		kept[i].size = fsize;
		kept[i].data = data;            /* (last: a reader of the table sees all of it or none) */
		*size = fsize;
	} else if (data != NULL) {
		kfree_data(data, (vm_size_t)fsize);
		data = NULL;
	}
out:
	lck_mtx_unlock(&image_lock);
	if (vol != NULL) {
		kfree_type(struct apfs_vol, vol);
	}
	if (nx != NULL) {
		kfree_type(struct apfs_nx, nx);
	}
	return data;
}

#ifdef HAKIOS_UFS
static int
ufs_root_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	return hakios_ufs_read_partition((const char *)ctx, off, buf, len, NULL);
}

/* fw/NAME of the flash's root (ufs_root=1 ufs_part=NAME), or NULL */
static const uint8_t *
ufs_root_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	char part[36] = "";
	uint32_t root = 0, allowed = 1;
	uint64_t bytes = 0;
	struct apfs_dev dev = { .read = ufs_root_read, .read_data = ufs_root_read };
	const uint8_t *data;

	if (!PE_parse_boot_argn("ufs_root", &root, sizeof(root)) || root != 1 ||
	    (PE_parse_boot_argn("image_ufs", &allowed, sizeof(allowed)) && allowed == 0) ||
	    !PE_parse_boot_argn("ufs_part", part, sizeof(part)) || part[0] == 0 ||
	    hakios_ufs_read_partition(part, 0, NULL, 0, &bytes) != 0 || bytes == 0) {
		return NULL;
	}
	dev.ctx = part;
	dev.size = bytes;
	data = apfs_file(UFS_ROOT_DISK, &dev, NULL, dir, name, size);
	if (data != NULL && disk_out != NULL) {
		*disk_out = UFS_ROOT_DISK;
	}
	return data;
}
#endif

const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	for (int disk = 1; disk >= 0; disk--) {
		vm_offset_t base = 0;
		uint64_t length = 0;
		const uint8_t *data;
		dev_t dev = mdevlookup(disk);
		struct mem_dev m;
		struct apfs_dev source = { .ctx = &m, .read = mem_read, .read_data = mem_read };

		if (dev == (dev_t)-1 || mdevgetkernelrange(dev, &base, &length) != 0) {
			continue;
		}
		m.base = (const uint8_t *)base;
		m.length = length;
		source.size = length;
		data = apfs_file(disk, &source, (const uint8_t *)base, dir, name, size);
		if (data != NULL) {
			if (disk_out != NULL) {
				*disk_out = disk;
			}
			return data;
		}
	}
#ifdef HAKIOS_UFS
	return ufs_root_file(dir, name, size, disk_out);
#else
	return NULL;
#endif
}
