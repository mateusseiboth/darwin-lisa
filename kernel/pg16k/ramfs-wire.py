#!/usr/bin/env python3
# darwin-lisa: ramfs, HakiOS's read-write in-memory filesystem, for /private/var.
# The APFS driver mounts read only, iOS keeps /private/var on a separate Data volume,
# and nothing may be written to the phone's flash, so the data volume is ramfs
# (empty every boot; init26 mounts it and lays down the iOS data-volume skeleton).
# Source: hackios-xnu-kernel/xnu/files/bsd/miscfs/ramfs/ramfs.c (copy kept in
# darwin-lisa/kernel/pg16k/ramfs/ramfs.c). Registration mirrors hakios.patch.
# Idempotent; run inside docker (root). --revert supported.
import os, shutil, sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
SRC = "/home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/pg16k/ramfs/ramfs.c"
DST = f"{T}/bsd/miscfs/ramfs/ramfs.c"

EDITS = {
    f"{T}/bsd/conf/files": [
        ("bsd/miscfs/apfs/apfs_vnops.c\t\toptional apfs\n",
         "bsd/miscfs/apfs/apfs_vnops.c\t\toptional apfs\nbsd/miscfs/ramfs/ramfs.c\t\toptional ramfs\n"),
    ],
    f"{T}/config/MASTER": [
        ("options\t\tAPFS\t\t# Apple File System (HakiOS built-in)\t# <apfs>\n",
         "options\t\tAPFS\t\t# Apple File System (HakiOS built-in)\t# <apfs>\n"
         "options\t\tRAMFS\t\t# Read-write filesystem in memory (darwin-lisa /private/var)\t# <ramfs>\n"),
    ],
    f"{T}/config/MASTER.arm64.bcm2837": [
        ("config_dataless_files bindfs apfs ]", "config_dataless_files bindfs apfs ramfs ]"),
    ],
    f"{T}/bsd/vfs/vfs_conf.c": [
        ("#if APFS\nextern  struct vfsops apfs_vfsops;\n",
         "#if RAMFS\nextern  struct vfsops ramfs_vfsops;\n#endif /* RAMFS */\n\n#if APFS\nextern  struct vfsops apfs_vfsops;\n"),
        ("\tFT_APFS    = 0x61706673\n", "\tFT_APFS    = 0x61706673,\n\tFT_RAMFS   = 0x72616D66\n"),
        ("#if APFS\n\t/*\n\t * Apple File System, read-write (HakiOS built-in)",
         "#if RAMFS\n"
         "\t/* darwin-lisa: read-write filesystem in memory (no device, no mount arguments) */\n"
         "\t{\n"
         "\t\t.vfc_vfsops = &ramfs_vfsops,\n"
         "\t\t.vfc_name = \"ramfs\",\n"
         "\t\t.vfc_typenum = FT_RAMFS,\n"
         "\t\t.vfc_refcount = 0,\n"
         "\t\t.vfc_flags = MNT_LOCAL,\n"
         "\t\t.vfc_mountroot = NULL,\n"
         "\t\t.vfc_next = NULL,\n"
         "\t\t.vfc_reserved1 = 0,\n"
         "\t\t.vfc_reserved2 = 0,\n"
         "\t\t.vfc_vfsflags = VFC_VFSGENERICARGS | VFC_VFS64BITREADY | VFC_VFSNATIVEXATTR,\n"
         "\t\t.vfc_descptr = NULL,\n"
         "\t\t.vfc_descsize = 0,\n"
         "\t\t.vfc_sysctl = NULL\n"
         "\t},\n"
         "#endif /* RAMFS */\n\n"
         "#if APFS\n\t/*\n\t * Apple File System, read-write (HakiOS built-in)"),
        ("#if APFS\nextern const struct vnodeopv_desc apfs_vnodeop_opv_desc;\n",
         "#if RAMFS\nextern const struct vnodeopv_desc ramfs_vnodeop_opv_desc;\n#endif /* RAMFS */\n\n"
         "#if APFS\nextern const struct vnodeopv_desc apfs_vnodeop_opv_desc;\n"),
        ("#if APFS\n\t&apfs_vnodeop_opv_desc,\n#endif /* APFS */\n\tNULL\n",
         "#if RAMFS\n\t&ramfs_vnodeop_opv_desc,\n#endif /* RAMFS */\n#if APFS\n\t&apfs_vnodeop_opv_desc,\n#endif /* APFS */\n\tNULL\n"),
    ],
}

revert = "--revert" in sys.argv
for path, edits in EDITS.items():
    with open(path) as f:
        d = f.read()
    for old, new in edits:
        if revert:
            d = d.replace(new, old)
        elif new in d:
            pass
        elif d.count(old) == 1:
            d = d.replace(old, new)
        else:
            print("ramfs: anchor missing in", path, old[:50].encode()); sys.exit(1)
    with open(path, "w") as f:
        f.write(d)
if revert:
    if os.path.exists(DST):
        os.remove(DST)
    print("ramfs: reverted")
else:
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    shutil.copyfile(SRC, DST)
    print("ramfs: wired")
