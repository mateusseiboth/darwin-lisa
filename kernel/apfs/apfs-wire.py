#!/usr/bin/env python3
# darwin-lisa: wire HakiOS APFS driver into the xnu-12377 work tree. Idempotent.
import os, shutil, sys

H = "/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel/xnu-src/xnu-8796.121.2"
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"

def edit(path, old, new, tag):
    with open(path) as f:
        s = f.read()
    if new in s and old not in s:
        print(f"  [skip] {tag}: already applied")
        return
    n = s.count(old)
    if n != 1:
        print(f"  [FAIL] {tag}: anchor found {n} times (need 1) in {path}")
        sys.exit(2)
    s = s.replace(old, new)
    with open(path, "w") as f:
        f.write(s)
    print(f"  [ok]   {tag}")

# 1. copy apfs source tree
dst = os.path.join(T, "bsd/miscfs/apfs")
if os.path.isdir(dst):
    shutil.rmtree(dst)
shutil.copytree(os.path.join(H, "bsd/miscfs/apfs"), dst)
print("  [ok]   copied bsd/miscfs/apfs ->", dst)

# 2. bsd/conf/files : add the 7 apfs compilation units
files = os.path.join(T, "bsd/conf/files")
apfs_block = (
    "\n# HakiOS APFS driver (built-in, read-write VFS), ported from xnu-8796\n"
    "bsd/miscfs/apfs/apfs_deflate.c\t\toptional apfs\n"
    "bsd/miscfs/apfs/apfs_zbitmap.c\t\toptional apfs\n"
    "bsd/miscfs/apfs/apfs_lib.c\t\toptional apfs\n"
    "bsd/miscfs/apfs/apfs_write.c\t\toptional apfs\n"
    "bsd/miscfs/apfs/apfs_store.c\t\toptional apfs\n"
    "bsd/miscfs/apfs/apfs_vfsops.c\t\toptional apfs\n"
    "bsd/miscfs/apfs/apfs_vnops.c\t\toptional apfs\n"
)
with open(files) as f:
    fc = f.read()
if "bsd/miscfs/apfs/apfs_vfsops.c" not in fc:
    with open(files, "a") as f:
        f.write(apfs_block)
    print("  [ok]   bsd/conf/files += apfs sources")
else:
    print("  [skip] bsd/conf/files: apfs already listed")

# 3. bsd/conf/Makefile.arm64 : libdeflate NEON header path
edit(os.path.join(T, "bsd/conf/Makefile.arm64"),
     "kern_csr.o_INCFLAGS += -I$(SRCROOT)/osfmk",
     "# libdeflate's arm headers (bsd/miscfs/apfs/libdeflate) include the compiler's own\n"
     "# <arm_acle.h> and <arm_neon.h>; -idirafter puts the resource dir after the kernel's\n"
     "# own include paths so the kernel's <stdint.h> et al are still the ones found.\n"
     "apfs_deflate.o_CFLAGS_ADD += -idirafter $(shell $(CC) -print-resource-dir)/include\n\n"
     "kern_csr.o_INCFLAGS += -I$(SRCROOT)/osfmk",
     "Makefile.arm64 apfs_deflate idirafter")

# 4. config/MASTER : declare the APFS option
edit(os.path.join(T, "config/MASTER"),
     "options\t\tMOCKFS\t\t# Boot from an executable\t# <mockfs>",
     "options\t\tMOCKFS\t\t# Boot from an executable\t# <mockfs>\n"
     "options\t\tAPFS\t\t# Apple File System (HakiOS built-in)\t# <apfs>",
     "config/MASTER options APFS")

# 5. config/MASTER.arm64.bcm2837 : add apfs tag to FILESYS_BASE
edit(os.path.join(T, "config/MASTER.arm64.bcm2837"),
     "#  FILESYS_BASE =   [ devfs fifo fs_compression config_protect config_mnt_rootsnap config_triggers config_fse routefs namedstreams config_dataless_files bindfs]",
     "#  FILESYS_BASE =   [ devfs fifo fs_compression config_protect config_mnt_rootsnap config_triggers config_fse routefs namedstreams config_dataless_files bindfs apfs ]",
     "MASTER.arm64.bcm2837 FILESYS_BASE += apfs")

# 6. bsd/vfs/vfs_conf.c : register apfs
vc = os.path.join(T, "bsd/vfs/vfs_conf.c")
# 6A externs
edit(vc,
     "#if MOCKFS\nextern  struct vfsops mockfs_vfsops;\nextern  int mockfs_mountroot(mount_t, vnode_t, vfs_context_t);\n#endif /* MOCKFS */\n",
     "#if MOCKFS\nextern  struct vfsops mockfs_vfsops;\nextern  int mockfs_mountroot(mount_t, vnode_t, vfs_context_t);\n#endif /* MOCKFS */\n\n"
     "#if APFS\nextern  struct vfsops apfs_vfsops;\nextern  int apfs_mountroot(mount_t, vnode_t, vfs_context_t);\n#endif /* APFS */\n",
     "vfs_conf.c apfs vfsops extern")
# 6B FT enum
edit(vc,
     "\tFT_MOCKFS  = 0x6D6F636B\n};",
     "\tFT_MOCKFS  = 0x6D6F636B,\n\tFT_APFS    = 0x61706673\n};",
     "vfs_conf.c FT_APFS enum")
# 6C vfsconf entry (apfs before mockfs)
edit(vc,
     "#if MOCKFS\n\t/* If we are configured for it, mockfs should always be the last standard entry (and thus the last FS we attempt mountroot with) */\n\t{\n\t\t.vfc_vfsops = &mockfs_vfsops,",
     "#if APFS\n"
     "\t/*\n"
     "\t * Apple File System, read-write (HakiOS built-in): the RAM-disk root and images are\n"
     "\t * APFS containers. Tried before mockfs; a non-APFS device returns EINVAL and\n"
     "\t * vfs_mountroot() moves on. Generic (not VFSLOCALARGS) so apfs_mount() can run from\n"
     "\t * the kernel and open the device path itself.\n"
     "\t */\n"
     "\t{\n"
     "\t\t.vfc_vfsops = &apfs_vfsops,\n"
     "\t\t.vfc_name = \"apfs\",\n"
     "\t\t.vfc_typenum = FT_APFS,\n"
     "\t\t.vfc_refcount = 0,\n"
     "\t\t.vfc_flags = MNT_LOCAL,\n"
     "\t\t.vfc_mountroot = apfs_mountroot,\n"
     "\t\t.vfc_next = NULL,\n"
     "\t\t.vfc_reserved1 = 0,\n"
     "\t\t.vfc_reserved2 = 0,\n"
     "\t\t.vfc_vfsflags = VFC_VFSGENERICARGS | VFC_VFS64BITREADY | VFC_VFSNATIVEXATTR | VFC_VFSREADDIR_EXTENDED,\n"
     "\t\t.vfc_descptr = NULL,\n"
     "\t\t.vfc_descsize = 0,\n"
     "\t\t.vfc_sysctl = NULL\n"
     "\t},\n"
     "#endif /* APFS */\n\n"
     "#if MOCKFS\n\t/* If we are configured for it, mockfs should always be the last standard entry (and thus the last FS we attempt mountroot with) */\n\t{\n\t\t.vfc_vfsops = &mockfs_vfsops,",
     "vfs_conf.c apfs vfsconf entry")
# 6D vnodeopv extern
edit(vc,
     "#if MOCKFS\nextern const struct vnodeopv_desc mockfs_vnodeop_opv_desc;\n#endif /* MOCKFS */\n",
     "#if MOCKFS\nextern const struct vnodeopv_desc mockfs_vnodeop_opv_desc;\n#endif /* MOCKFS */\n\n"
     "#if APFS\nextern const struct vnodeopv_desc apfs_vnodeop_opv_desc;\n#endif /* APFS */\n",
     "vfs_conf.c apfs vnodeopv extern")
# 6E vfs_opv_descs array
edit(vc,
     "#if MOCKFS\n\t&mockfs_vnodeop_opv_desc,\n#endif /* MOCKFS */\n\tNULL",
     "#if MOCKFS\n\t&mockfs_vnodeop_opv_desc,\n#endif /* MOCKFS */\n#if APFS\n\t&apfs_vnodeop_opv_desc,\n#endif /* APFS */\n\tNULL",
     "vfs_conf.c apfs opv_descs array")

# 7. apfs_vfsops.c : shim HakiOS device predicates + guard cosmetic sysctl
vf = os.path.join(dst, "apfs_vfsops.c")
edit(vf,
     "#include <sys/hakios.input.h>\n#include <sys/hakios.virtio_blk.h>\n",
     "/* darwin-lisa: HakiOS device-type predicates. The root device in this tree is the md0\n"
     " * RAM disk (bsd/dev/memdev.c); the HakiOS ufs and virtio_blk drivers are not present, so\n"
     " * both predicates are false and cheap_requests ends up true, correct for a RAM disk. */\n"
     "static inline bool hakios_ufs_dev(dev_t dev) { (void)dev; return false; }\n"
     "static inline bool hakios_virtio_blk_dev(dev_t dev) { (void)dev; return false; }\n",
     "apfs_vfsops.c hakios predicate shim")
edit(vf,
     "/* kern.hakios_memory: the kernel's line about memory (osfmk/arm64/loader_return.c), for a job list to keep */\n"
     "extern void hakios_memory_line(char *buf, size_t len);\n\n"
     "static int\n"
     "sysctl_hakios_memory SYSCTL_HANDLER_ARGS\n"
     "{\n"
     "#pragma unused(oidp, arg1, arg2)\n"
     "\tchar line[256];\n\n"
     "\thakios_memory_line(line, sizeof(line));\n"
     "\treturn SYSCTL_OUT(req, line, strlen(line) + 1);\n"
     "}\n\n"
     "SYSCTL_PROC(_kern, OID_AUTO, hakios_memory, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,\n"
     "    0, 0, sysctl_hakios_memory, \"A\", \"where the memory is, in MB\");\n",
     "#if 0 /* darwin-lisa: kern.hakios_memory depends on HakiOS loader_return.c; cosmetic, not needed for APFS */\n"
     "/* kern.hakios_memory: the kernel's line about memory (osfmk/arm64/loader_return.c), for a job list to keep */\n"
     "extern void hakios_memory_line(char *buf, size_t len);\n\n"
     "static int\n"
     "sysctl_hakios_memory SYSCTL_HANDLER_ARGS\n"
     "{\n"
     "#pragma unused(oidp, arg1, arg2)\n"
     "\tchar line[256];\n\n"
     "\thakios_memory_line(line, sizeof(line));\n"
     "\treturn SYSCTL_OUT(req, line, strlen(line) + 1);\n"
     "}\n\n"
     "SYSCTL_PROC(_kern, OID_AUTO, hakios_memory, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,\n"
     "    0, 0, sysctl_hakios_memory, \"A\", \"where the memory is, in MB\");\n"
     "#endif\n",
     "apfs_vfsops.c guard hakios_memory sysctl")

print("WIRE DONE")
