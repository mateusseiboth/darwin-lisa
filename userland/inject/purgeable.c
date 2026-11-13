/*
 * libdlisa, PosterBoard: marking a file purgeable on the HFSX data volume.
 *
 * Purgeable files are an APFS feature: PosterBoard's -[NSURL pbf_setPurgable:error:] is
 * ffsctl(fd, APFSIOC_MARK_PURGEABLE = 0xc0084a44, &flags, 0), and PosterFoundation's
 * -[NSURL pf_setPurgable:afterDate:error:] and -[NSURL pf_unmarkAsPurgableWithError:] are fsctl with the APFS
 * purgeable commands (0x40184a03, 0xc1104a71, 0xc0304a6f, 0xc0084a44). HFS knows none of them and the call returns -1.
 *
 * PosterBoard's PBFPosterModelStoreCoordinator -ensureFileSystemIntegrity (via
 * _accessQueue_correctPermissionsForInternalDirectories:) unmarks every file of a new poster descriptor as purgeable
 * and returns that error, so +createNewStoreCoordinatorWithPath:error: fails and no static descriptor, hence no poster,
 * is ever made ("Unable to create new static descriptor path", host run poster-f1). On a volume without purgeable
 * state a file is never purgeable: unmarking is already done, and marking has nothing to record.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* fstypename: statfs's f_fstypename of the file's volume */
static bool
isPurgeableUnsupported(const char *fstypename)
{
	return fstypename != NULL && fstypename[0] != 0 && strcmp(fstypename, "apfs") != 0;
}
