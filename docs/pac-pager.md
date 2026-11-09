# The PAC-stripping pager

How the kernel rewrites arm64e code in memory, so that iOS 26 runs unchanged from its sealed volumes on a CPU without
pointer authentication. References are to Apple's open xnu-12377.121.6.

## The mechanism XNU already has

`osfmk/vm/vm_apple_protect.c` is a pager that sits between a mapping and the file behind it. On a page fault it reads
the page from the file and calls, for each 4 KiB of it:

```c
/* osfmk/kern/page_decrypt.h */
struct pager_crypt_info {
	int  (*page_decrypt)(const void *src_vaddr, void *dst_vaddr, unsigned long long src_offset, void *crypt_ops);
	void (*crypt_end)(void *crypt_ops);
	void *crypt_ops;
	volatile int crypt_refcnt;
};
```

`vm_map_apple_protected(map, start, end, backing_offset, &crypt_info, cryptid)` puts a range of an existing mapping
behind that pager. FairPlay uses it for encrypted apps (`set_code_unprotect()` in `bsd/kern/mach_loader.c`), and the
old DSMOS scheme for protected segments (`unprotect_dsmos_segment()`, same file): a fixed transform, applied while an
executable's segments are mapped. The PAC pager is the DSMOS case with another transform.

## The transform

```c
static int pac_page_transform(const void *src, void *dst, unsigned long long offset, void *ops)
{
	memcpy(dst, src, PAGE_SIZE_4K);
	pac_rewrite_buffer(dst, PAGE_SIZE_4K / 4);	/* kernel/pacstrip/pac_rewrite.c */
	return 0;
}
```

`ops` can carry the file's `LC_DATA_IN_CODE` ranges, to leave them alone, if the scan of the real cache shows any
inside a code section.

## Where it is applied

| What | Where it is mapped | Hook |
| --- | --- | --- |
| The main executable and dyld | `map_segment()` in `bsd/kern/mach_loader.c`, the call site of `unprotect_dsmos_segment()` | every `VM_PROT_EXECUTE` segment of a Mach-O whose `cpusubtype` is `CPU_SUBTYPE_ARM64E` |
| Dylibs outside the shared cache, plug-ins | `mmap()` in `bsd/kern/kern_mman.c` (dyld maps their `__TEXT` with `PROT_EXEC`) | an executable file mapping whose first page is an arm64e Mach-O header |
| The dyld shared cache | `vm_shared_region_map_file_setup()` in `osfmk/vm/vm_shared_region.c` | every mapping with `VM_PROT_EXECUTE` in its max protection, once, in the shared region's own map: every process then shares the rewritten pages |

Code signing: the pager hands out pages that differ from the signed ones, so the kernel runs with code-signing
enforcement off for these mappings (the port's AMFI policy already does).

## Not done by the pager

- **Kexts.** They are prelinked into the kernelcache at build time and rewritten then with `tools/pacstrip`.
- **Instructions without a 4-byte replacement** (`LDRAA` out of `LDUR` range, `FJCVTZS`, and whatever
  `featscan.py` finds beyond the A78): the pager leaves them; the undefined-instruction handler emulates them.
- **The PAC key registers.** A kernel built without ptrauth never writes them; the arm64e process setup paths that
  do are patched out.

## Implementation status (2026-11-09)

Built and linked into the xnu-12377 kernel (`kernel/patches/apply.sh`, verified by the symbols in the
linked kernel):

- `pac_page_transform()` + `pac_protect_segment()` live in `bsd/kern/mach_loader.c`
  (`patches/mach_loader-pac-pager.patch`); the crypter calls `pac_rewrite_buffer()` (`pac_rewrite.c`,
  now compiled into osfmk). `load_segment()` applies it to every `VM_PROT_EXECUTE` segment of an
  arm64e Mach-O — the **main executable and dyld**.
- The loader accepts the arm64e slice (`patches/machdep-grade-arm64e.patch`).

Remaining pager surface (needed before launchd fully runs):

- the **dyld shared cache** in `osfmk/vm/vm_shared_region.c` (`vm_shared_region_map_file_setup`): rewrite
  each executable mapping once, in the shared region's own map, so every process shares the result;
- non-cache dylibs / plug-ins via `mmap` in `bsd/kern/kern_mman.c`;
- turning off code-signing enforcement for these mappings (the pages differ from the signed ones).

Behavioural validation happens at the QEMU boot (the crypter, the emulator's EL0 traps, and the data-in-code
question are all exercised there, not by a compile).
