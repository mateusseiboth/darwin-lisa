#!/usr/bin/env python3
# darwin-lisa: strip pointer authentication from EXECUTABLE FILE MAPPINGS made with
# mmap(2), i.e. every arm64e dylib/bundle dyld loads from disk outside the shared
# cache (UserEventAgent / configd plugins, ...). mach_loader's pac pager only covers
# binaries the kernel itself execs, and phase 3 only the shared cache, so these
# trapped on the first BRAA/BLRAA/RETAB (EXC_BAD_INSTRUCTION, subcode 0xd71f0a11 =
# "braa x16, x17"). Same apple-protect pager + pac_rewrite_buffer, 4 KB per call.
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/bsd/kern/kern_mman.c"

HELPER_ANCHOR = "int\nmmap(proc_t p, struct mmap_args *uap, user_addr_t *retval)\n{\n"
HELPER = """#if CONFIG_CODE_DECRYPTION
/*
 * darwin-lisa: no pointer authentication on this CPU. An executable file mapping
 * (dyld loading an arm64e dylib or bundle that is not in the shared cache) is
 * re-backed by the apple-protect pager with a transform that rewrites PAC
 * instructions to their plain forms as pages come in (same as mach_loader.c).
 */
extern size_t pac_rewrite_buffer(uint32_t *words, size_t count);
extern kern_return_t vm_map_apple_protected(vm_map_t map, vm_map_offset_t start,
    vm_map_offset_t end, vm_object_offset_t crypto_backing_offset,
    struct pager_crypt_info *crypt_info, uint32_t cryptid);

static int
dlisa_mmap_pac_transform(const void *src, void *dst,
    unsigned long long src_offset __unused, void *ops __unused)
{
	memcpy(dst, src, 4096);
	pac_rewrite_buffer((uint32_t *)dst, 4096 / sizeof(uint32_t));
	return 0;
}

static void
dlisa_mmap_pac_protect(vm_map_t map, vm_map_offset_t addr, vm_map_size_t size)
{
	struct pager_crypt_info crypt_info;
	kern_return_t kr;

	if (size == 0) {
		return;
	}
	crypt_info.page_decrypt = dlisa_mmap_pac_transform;
	crypt_info.crypt_end = NULL;
	crypt_info.crypt_ops = NULL;
	crypt_info.crypt_refcnt = 0;
	kr = vm_map_apple_protected(map, addr, addr + size,
	    (vm_object_offset_t)-1, &crypt_info, 1 /* CRYPTID_APP_ENCRYPTION */);
	if (kr != KERN_SUCCESS) {
		printf("DLISA mmap pac: vm_map_apple_protected(0x%llx, 0x%llx) = %d\\n",
		    (unsigned long long)addr, (unsigned long long)size, kr);
	}
}
#endif /* CONFIG_CODE_DECRYPTION */

"""

CALL_ANCHOR = """		if ((result == KERN_NO_SPACE) && ((flags & MAP_FIXED) == 0) && user_addr && (num_retries++ == 0)) {
			user_addr = vm_map_page_size(user_map);
			goto map_file_retry;
		}
"""
CALL = """#if CONFIG_CODE_DECRYPTION
		/* darwin-lisa: PAC-strip executable private file mappings (dyld-loaded code) */
		if (result == KERN_SUCCESS && (prot & VM_PROT_EXECUTE) && !(flags & MAP_SHARED)) {
			dlisa_mmap_pac_protect(user_map, user_addr, user_size);
		}
#endif /* CONFIG_CODE_DECRYPTION */
"""

with open(F) as f:
    d = f.read()
if "--revert" in sys.argv:
    d = d.replace(HELPER + HELPER_ANCHOR, HELPER_ANCHOR).replace(CALL_ANCHOR + CALL, CALL_ANCHOR)
    print("mmappac: reverted")
elif "dlisa_mmap_pac_protect(user_map" in d:
    print("mmappac: already present")
else:
    for a in (HELPER_ANCHOR, CALL_ANCHOR):
        if d.count(a) != 1:
            print("mmappac: anchor missing/not unique:", a[:50].encode()); sys.exit(1)
    d = d.replace(HELPER_ANCHOR, HELPER + HELPER_ANCHOR, 1)
    d = d.replace(CALL_ANCHOR, CALL_ANCHOR + CALL, 1)
    print("mmappac: patched")
with open(F, "w") as f:
    f.write(d)
