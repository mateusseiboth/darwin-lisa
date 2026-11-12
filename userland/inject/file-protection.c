/*
 * libdlisa, PosterBoard: what NSURLFileProtectionKey means on the HFSX data volume.
 *
 * /private/var is HFS Plus built into the kernel with CONFIG_PROTECT 0 (bsd/hfs/kext-config.h): no content protection,
 * no MNT_CPROTECT, every file readable without a key. getattrlist's ATTR_CMN_DATA_PROTECT_FLAGS is therefore not
 * supported and comes back as class 0, and CoreServicesInternal's getter (classes 1..4 to Complete, CompleteUnlessOpen,
 * CompleteUntilFirstUserAuthentication, None) answers nil: Apple's own HFS does the same on an unprotected volume.
 *
 * PosterBoard 23F84 takes nil as "not conforming": its data store viability check
 * (_PBFDetermineDataStoreViabilityForVersionWithinBaseURL) wants every store directory and the SQLite file to read
 * {NSURLIsReadableKey: YES, NSURLFileProtectionKey: NSURLFileProtectionNone} (PosterFoundation's
 * PFPosterPathURLResourceValues, compared with BSEqualObjects). The store at version 59 is never viable, the 59 to 61
 * migration stops at its first step with no NSError, and the data store is never opened: no wallpaper, no lock screen
 * or Notification Center clock. On a volume without content protection "no class" is exactly "None".
 */
#include <stdbool.h>
#include <stdint.h>

/* <sys/mount.h>'s MNT_CPROTECT: the volume supports per-file content protection (the host test has no Darwin header) */
#define DLISA_MNT_CPROTECT 0x00000080

/* succeeded: the original lookup returned YES; valueIsNil: it gave no class; volumeFlags: statfs's f_flags */
static bool
isProtectionNoneImplied(bool succeeded, bool valueIsNil, uint64_t volumeFlags)
{
	return succeeded && valueIsNil && (volumeFlags & DLISA_MNT_CPROTECT) == 0;
}
