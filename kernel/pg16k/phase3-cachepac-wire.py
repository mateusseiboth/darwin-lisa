#!/usr/bin/env python3
# darwin-lisa PHASE 3: strip pointer authentication from the arm64e dyld shared
# cache's executable pages as they page in, so Apple's cache runs on this no-PAC
# CPU. The cache is mapped straight from the file into the shared-region submap
# (vm_shared_region_map_file_setup), bypassing the exec'd-binary pac pager in
# mach_loader.c, so its BLRAA/BRAA/RETAA/LDRAA etc. would trap. We wrap each
# executable cache mapping with the same PAC-stripping apple-protect pager.
#
# The apple-protect pager drives page_decrypt() in 4 KB sub-chunks
# (vm_apple_protect.c: offset_in_page += 4096), so the transform handles exactly
# 4096 bytes per call (same as mach_loader.c's pac_page_transform).
#
# Idempotent; run inside docker (root). Revert with --revert.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/osfmk/vm/vm_shared_region.c"

INC_ANCHOR = "#include <sys/code_signing.h>\n"
INC_ADD = "#include <kern/page_decrypt.h>\t/* darwin-lisa: PAC-strip shared cache */\n"

HELPERS_ANCHOR = "static kern_return_t\nvm_shared_region_map_file_setup(\n"
HELPERS = """#if CONFIG_CODE_DECRYPTION
/*
 * darwin-lisa: PAC-strip the arm64e dyld shared cache's executable pages on
 * page-in (no pointer authentication on this CPU). Mirrors mach_loader.c's
 * pac_protect_segment; the apple-protect pager calls page_decrypt() with 4 KB
 * sub-chunks, so this transforms 4096 bytes per call.
 */
extern size_t pac_rewrite_buffer(uint32_t *words, size_t count);

static int
shared_region_pac_page_transform(const void *src, void *dst,
    unsigned long long src_offset __unused, void *ops __unused)
{
\tmemcpy(dst, src, 4096);
\tpac_rewrite_buffer((uint32_t *)dst, 4096 / sizeof(uint32_t));
\treturn 0;
}

static void
pac_protect_shared_segment(vm_map_t map, vm_map_offset_t addr, vm_map_size_t size)
{
\tstruct pager_crypt_info crypt_info;

\tif (size == 0) {
\t\treturn;
\t}
\tcrypt_info.page_decrypt = shared_region_pac_page_transform;
\tcrypt_info.crypt_end = NULL;
\tcrypt_info.crypt_ops = NULL;
\tcrypt_info.crypt_refcnt = 0;
\t(void) vm_map_apple_protected(map, addr, addr + size,
\t    (vm_object_offset_t)-1, &crypt_info, 1 /* CRYPTID_APP_ENCRYPTION */);
}
#endif /* CONFIG_CODE_DECRYPTION */

static kern_return_t
vm_shared_region_map_file_setup(
"""

CALL_ANCHOR = """\t\t\t\tif (shared_region->sr_first_mapping == (mach_vm_offset_t)-1) {
\t\t\t\t\tshared_region->sr_first_mapping = target_address;
\t\t\t\t}
"""
CALL_ADD = """
#if CONFIG_CODE_DECRYPTION
\t\t\t\t/* darwin-lisa: PAC-strip executable shared-cache pages on page-in */
\t\t\t\tif (map_port != MACH_PORT_NULL &&
\t\t\t\t    (mappings[i].sms_max_prot & VM_PROT_EXECUTE)) {
\t\t\t\t\tpac_protect_shared_segment(sr_map, target_address,
\t\t\t\t\t    vm_map_round_page(mappings[i].sms_size,
\t\t\t\t\t    VM_MAP_PAGE_MASK(sr_map)));
\t\t\t\t}
#endif /* CONFIG_CODE_DECRYPTION */
"""

revert = "--revert" in sys.argv
with open(F) as f:
    d = f.read()

if revert:
    d = d.replace(INC_ANCHOR + INC_ADD, INC_ANCHOR)
    d = d.replace(HELPERS, HELPERS_ANCHOR)
    d = d.replace(CALL_ANCHOR + CALL_ADD, CALL_ANCHOR)
    print("phase3: reverted")
else:
    # include
    if INC_ADD in d:
        print("  = include present")
    elif INC_ANCHOR in d:
        d = d.replace(INC_ANCHOR, INC_ANCHOR + INC_ADD, 1)
        print("  + include added")
    else:
        print("  ! include anchor missing"); sys.exit(1)
    # helpers
    if "pac_protect_shared_segment(vm_map_t map" in d:
        print("  = helpers present")
    elif HELPERS_ANCHOR in d:
        d = d.replace(HELPERS_ANCHOR, HELPERS, 1)
        print("  + helpers added")
    else:
        print("  ! helpers anchor missing"); sys.exit(1)
    # call site
    if "pac_protect_shared_segment(sr_map, target_address" in d:
        print("  = call present")
    elif CALL_ANCHOR in d:
        d = d.replace(CALL_ANCHOR, CALL_ANCHOR + CALL_ADD, 1)
        print("  + call added")
    else:
        print("  ! call anchor missing"); sys.exit(1)

with open(F, "w") as f:
    f.write(d)
print("phase3 wire done")
