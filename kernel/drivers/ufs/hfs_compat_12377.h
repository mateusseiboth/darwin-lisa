/*
 * darwin-lisa: HFS Plus (Apple's hfs-627.100.6 as HakiOS built it into xnu-8796) on xnu-12377.
 *
 * Forced in before every bsd/hfs file (bsd/conf/Makefile.template, datavol-wire.py). Checked offline: with these two,
 * every file of bsd/hfs compiles against this tree's exported headers, and every symbol the objects need is defined
 * in the iOS 26 kernel (kernel26.nopac). Nothing else of xnu changed under HFS between the two.
 *
 * xnu-12377 removed the snapshot side of the namespace handler (nspace_snapshot_event) and the old entry point for
 * dataless files (resolve_nspace_item; materializing is vfs_materialize_file now). Neither has any use here:
 *   - snapshot events were for a backup agent watching the volume; with nobody registered they did nothing;
 *   - a dataless file (compressed type DATALESS_CMPFS_TYPE) never exists on a volume this port makes, and if one
 *     ever did, EIO is what HFS answers when its handler fails.
 */
#ifndef DARWIN_LISA_HFS_COMPAT_12377_H
#define DARWIN_LISA_HFS_COMPAT_12377_H

#define nspace_snapshot_event(vp, ctime, op, arg) ((void)(vp), (void)(ctime), (void)(op), (void)(arg), 0)
#define resolve_nspace_item(vp, op)               ((void)(vp), (void)(op), EIO)

#endif
