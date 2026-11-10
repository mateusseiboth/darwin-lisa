#!/usr/bin/env python3
# darwin-lisa: the persistent data volume (/private/var on the phone's flash), kernel side. NOT applied by anything
# else; run it by hand, in docker as root, AFTER drivers/hakios-wire.py and kernel/drivers/ufs/ufs-wire.py, then
# .work/run-rebuild.sh. Idempotent. XNU_WORK=<dir> points it at another tree. See docs/data-volume.md.
#
#   1. bsd/dev/hakios.ufs.c   datavol.patch: /dev/ufs_data, the region the loader offers (ufs_write=1 ufs_data=NAME),
#                             bounded by the loader's [write_first, write_end); no character device writes any more;
#                             with ufs_native=1 the kernel's list goes back in after the writes (ufs_data_restore_ms)
#   2. bsd/hfs, bsd/hfs_encodings
#                             HFS Plus, read-write, built in: Apple's hfs-627.100.6 + HakiOS's hfs.patch +
#                             hfs_builtin.c, exactly as HakiOS built it into xnu-8796 (checked by a hash of the result),
#                             plus hfs_compat_12377.h (two KPIs xnu-12377 removed)
#   3. bsd/conf/files, bsd/conf/Makefile.template, config/MASTER, config/MASTER.arm64.bcm2837, bsd/vfs/vfs_conf.c
#                             the "hfs" option, compiled like its kext was (KPI only, kext-config.h first), and the
#                             filesystem in the static table, so init can mount("hfs", "/private/var", ...)
#
# Env: HFS_SRC (default research/hackios-xnu-kernel/xnu-src/hfs-627.100.6), HAKIOS (research/hackios-xnu-kernel).
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
M = os.path.abspath(os.path.join(HERE, "../../../.."))
T = os.environ.get("XNU_WORK", M + "/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work")
HAKIOS = os.environ.get("HAKIOS", M + "/research/hackios-xnu-kernel")
HFS_SRC = os.environ.get("HFS_SRC", HAKIOS + "/xnu-src/hfs-627.100.6")
# sha256 over (name, bytes) of bsd/hfs/* and bsd/hfs_encodings/*, sorted: HakiOS's bsd/hfs of xnu-8796, byte for byte
HFS_TREE_SHA = "896bc965c04608185ee866bf7a733418b27fd5beba490e6f0fba637bad867560"


def edit(path, old, new, marker, tag):
    with open(path) as f:
        s = f.read()
    if marker in s:
        print(f"  [skip] {tag}: already applied")
        return
    n = s.count(old)
    if n != 1:
        print(f"  [FAIL] {tag}: anchor found {n} times (need 1) in {path}")
        sys.exit(2)
    with open(path, "w") as f:
        f.write(s.replace(old, new))
    print(f"  [ok]   {tag}")


def tree_sha(root):
    h = hashlib.sha256()
    for d in ("bsd/hfs", "bsd/hfs_encodings"):
        for f in sorted(os.listdir(os.path.join(root, d))):
            if f == "hfs_compat_12377.h":
                continue
            h.update((d + "/" + f).encode())
            with open(os.path.join(root, d, f), "rb") as fh:
                h.update(fh.read())
    return h.hexdigest()


# 1. the driver
ufs = os.path.join(T, "bsd/dev/hakios.ufs.c")
with open(ufs) as f:
    if "ufs_data_init" in f.read():
        print("  [skip] bsd/dev/hakios.ufs.c: datavol.patch already applied")
    else:
        subprocess.run(["patch", "-p1", "--forward", "-i", os.path.join(HERE, "datavol.patch")], cwd=T, check=True)
        print("  [ok]   bsd/dev/hakios.ufs.c: datavol.patch (run ufs-wire.py first: it copies the driver)")

# 2. HFS Plus sources
if not os.path.exists(os.path.join(T, "bsd/hfs/hfs_vfsops.c")):
    for d, lst in (("bsd/hfs", "core.list"), ("bsd/hfs_encodings", "encodings.list")):
        os.makedirs(os.path.join(T, d), exist_ok=True)
        src_dir = os.path.join(HFS_SRC, "core" if d == "bsd/hfs" else "hfs_encodings")
        with open(os.path.join(HAKIOS, "xnu/hfs", lst)) as f:
            for name in f.read().split():
                shutil.copyfile(os.path.join(src_dir, name), os.path.join(T, d, name))
    subprocess.run(["patch", "-p1", "--forward", "-s", "-i", os.path.join(HAKIOS, "xnu/hfs/hfs.patch")], cwd=T,
                   check=True)
    shutil.copyfile(os.path.join(HAKIOS, "xnu/files/bsd/hfs/hfs_builtin.c"), os.path.join(T, "bsd/hfs/hfs_builtin.c"))
    print("  [ok]   bsd/hfs, bsd/hfs_encodings: hfs-627.100.6 + hfs.patch + hfs_builtin.c")
else:
    print("  [skip] bsd/hfs: already there")
got = tree_sha(T)
if got != HFS_TREE_SHA:
    print(f"  [FAIL] bsd/hfs is not HakiOS's (sha {got}): remove bsd/hfs and bsd/hfs_encodings and run again")
    sys.exit(2)
shutil.copyfile(os.path.join(HERE, "hfs_compat_12377.h"), os.path.join(T, "bsd/hfs/hfs_compat_12377.h"))
print("  [ok]   bsd/hfs is HakiOS's, byte for byte; + hfs_compat_12377.h")

# 3a. bsd/conf/files
files = os.path.join(T, "bsd/conf/files")
hfs_c = sorted(f for f in os.listdir(os.path.join(T, "bsd/hfs")) if f.endswith(".c"))
edit(files, "bsd/miscfs/ramfs/ramfs.c\t\toptional ramfs\n",
     "bsd/miscfs/ramfs/ramfs.c\t\toptional ramfs\n"
     "# darwin-lisa: HFS Plus, read-write, built in (the data volume on the flash, docs/data-volume.md)\n" +
     "".join(f"bsd/hfs/{c}\t\toptional hfs\n" for c in hfs_c),
     "bsd/hfs/hfs_vfsops.c", f"bsd/conf/files += {len(hfs_c)} bsd/hfs files")

# 3b. bsd/conf/Makefile.template: compiled as its kext was
edit(os.path.join(T, "bsd/conf/Makefile.template"), "%RULES\n\ninclude $(MakeInc_rule)\n",
     "%RULES\n\n"
     "# darwin-lisa (as HakiOS): HFS Plus built in, compiled as its kext was -- against the kernel's KPI, not its insides\n"
     "# (no XNU/BSD/MACH_KERNEL_PRIVATE), kext-config.h and hfs_compat_12377.h first, its own headers beside it, the\n"
     "# SDK's <TargetConditionals.h> after the kernel's paths, and its warnings its own\n"
     "HFS_OBJS = $(patsubst %.c,%.o,$(notdir $(wildcard $(SRCROOT)/bsd/hfs/*.c)))\n"
     "$(foreach o,$(HFS_OBJS),$(eval $(o)_CFLAGS_ADD += -UXNU_KERNEL_PRIVATE -UBSD_KERNEL_PRIVATE -UMACH_KERNEL_PRIVATE \\\n"
     "\t-include $(SRCROOT)/bsd/hfs/kext-config.h -include $(SRCROOT)/bsd/hfs/hfs_compat_12377.h -I$(SRCROOT)/bsd/hfs \\\n"
     "\t-idirafter $(SDKROOT_RESOLVED)/usr/include -idirafter $(SRCROOT)/../fakeroot/usr/include))\n"
     "$(foreach o,$(HFS_OBJS),$(eval $(o)_CWARNFLAGS_ADD += -w))\n"
     "\ninclude $(MakeInc_rule)\n",
     "HFS_OBJS =", "bsd/conf/Makefile.template: HFS objects' flags")

# 3c. config/MASTER and the DEVELOPMENT config of this board
ramfs_opt = "options\t\tRAMFS\t\t# Read-write filesystem in memory (darwin-lisa /private/var)\t# <ramfs>\n"
edit(os.path.join(T, "config/MASTER"), ramfs_opt,
     ramfs_opt + "options\t\tHFS\t\t# HFS Plus, read-write: the data volume on the flash (darwin-lisa)\t# <hfs>\n",
     "\t# <hfs>\n", "config/MASTER options HFS")
edit(os.path.join(T, "config/MASTER.arm64.bcm2837"), " apfs ramfs ]", " apfs ramfs hfs ]",
     " ramfs hfs ]", "MASTER.arm64.bcm2837 FILESYS_BASE += hfs")

# 3d. bsd/vfs/vfs_conf.c: the filesystem in the static table
vc = os.path.join(T, "bsd/vfs/vfs_conf.c")
edit(vc, "#if APFS\nextern  struct vfsops apfs_vfsops;\n",
     "#if HFS\nextern  struct vfsops hfs_vfsops;\n#endif /* HFS */\n\n#if APFS\nextern  struct vfsops apfs_vfsops;\n",
     "extern  struct vfsops hfs_vfsops;", "vfs_conf.c: extern hfs_vfsops")
edit(vc, "\tFT_RAMFS   = 0x72616D66\n};", "\tFT_RAMFS   = 0x72616D66,\n\tFT_HFS     = 0x11\n};",
     "FT_HFS     = 0x11", "vfs_conf.c: FT_HFS")
edit(vc, "#endif /* RAMFS */\n\n#if APFS\n\t/*\n",
     "#endif /* RAMFS */\n\n"
     "#if HFS\n"
     "\t/*\n"
     "\t * darwin-lisa (as HakiOS): HFS Plus, read-write, built in rather than a kext (bsd/hfs, Apple's hfs-627.100.6):\n"
     "\t * the data volume on the phone's flash, /private/var (/dev/ufs_data). Its own arguments (struct hfs_mount_args),\n"
     "\t * local ones as the kext registered itself: the kernel looks up the device named by their first field and hands\n"
     "\t * its vnode to hfs_mount(). Mounted by init26 from user space (var_ufs=1).\n"
     "\t */\n"
     "\t{\n"
     "\t\t.vfc_vfsops = &hfs_vfsops,\n"
     "\t\t.vfc_name = \"hfs\",\n"
     "\t\t.vfc_typenum = FT_HFS,\n"
     "\t\t.vfc_refcount = 0,\n"
     "\t\t.vfc_flags = MNT_LOCAL | MNT_DOVOLFS,\n"
     "\t\t.vfc_mountroot = NULL,\n"
     "\t\t.vfc_next = NULL,\n"
     "\t\t.vfc_reserved1 = 0,\n"
     "\t\t.vfc_reserved2 = 0,\n"
     "\t\t.vfc_vfsflags = VFC_VFSLOCALARGS | VFC_VFSREADDIR_EXTENDED | VFC_VFS64BITREADY | VFC_VFSVNOP_PAGEOUTV2 |\n"
     "\t\t    VFC_VFSVNOP_PAGEINV2 | VFC_VFSNATIVEXATTR,\n"
     "\t\t.vfc_descptr = NULL,\n"
     "\t\t.vfc_descsize = 0,\n"
     "\t\t.vfc_sysctl = NULL\n"
     "\t},\n"
     "#endif /* HFS */\n\n"
     "#if APFS\n\t/*\n",
     ".vfc_vfsops = &hfs_vfsops,", "vfs_conf.c: the hfs entry")
edit(vc, "#if APFS\nextern const struct vnodeopv_desc apfs_vnodeop_opv_desc;\n",
     "#if HFS\nextern const struct vnodeopv_desc hfs_vnodeop_opv_desc;\n"
     "extern const struct vnodeopv_desc hfs_specop_opv_desc;\n"
     "extern const struct vnodeopv_desc hfs_fifoop_opv_desc;\n#endif /* HFS */\n\n"
     "#if APFS\nextern const struct vnodeopv_desc apfs_vnodeop_opv_desc;\n",
     "extern const struct vnodeopv_desc hfs_vnodeop_opv_desc;", "vfs_conf.c: hfs vnodeopv declarations")
edit(vc, "#if APFS\n\t&apfs_vnodeop_opv_desc,\n#endif /* APFS */\n\tNULL\n};",
     "#if HFS\n\t&hfs_vnodeop_opv_desc,\n\t&hfs_specop_opv_desc,\n\t&hfs_fifoop_opv_desc,\n#endif /* HFS */\n"
     "#if APFS\n\t&apfs_vnodeop_opv_desc,\n#endif /* APFS */\n\tNULL\n};",
     "\t&hfs_vnodeop_opv_desc,", "vfs_conf.c: hfs vnodeopv in vfs_opv_descs")

print("### DATAVOL WIRE DONE (now .work/run-rebuild.sh, then grep 'error:' in logs/kernel.log)")
