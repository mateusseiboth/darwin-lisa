#!/usr/bin/env python3
# darwin-lisa: dyld page-in linking (osfmk/vm/vm_dyld_pager.c) only knows the arm64e
# chained-fixup formats (ARM64E, ARM64E_USERLAND, ARM64E_USERLAND24, ARM64E_SHARED_CACHE)
# under HAS_APPLE_PAC. This CPU has no PAC and the kernel does not define it, so every
# arm64e binary's DATA_CONST/AUTH page-in failed with
#   fixup_page(): unknown pointer_format 12
# and Apple's launchd died with SIGBUS. Compile the arm64e paths in anyway, with a
# signPointer() that stores the pointer unsigned (what a pointer is without PAC).
# Idempotent; run inside docker (root). --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = f"{T}/osfmk/vm/vm_dyld_pager.c"

EDITS = [
    # 1. the switch for it
    ("#include <arm/misc_protos.h>\n#endif /* defined(HAS_APPLE_PAC) */\n",
     "#include <arm/misc_protos.h>\n#endif /* defined(HAS_APPLE_PAC) */\n"
     "/* darwin-lisa: no PAC, but arm64e binaries still carry arm64e chained fixups */\n"
     "#if !defined(HAS_APPLE_PAC) && defined(__arm64__)\n"
     "#define DLISA_ARM64E_FIXUPS 1\n"
     "#include <ptrauth.h>\n"
     "#endif\n"),
    # 2. an unsigned signPointer() for the no-PAC build, then open the arm64e fixup block
    ("\treturn KERN_SUCCESS;\n}\n\n/*\n * Apply fixups to a page used by a 64 bit process using pointer authentication.\n */\n"
     "static kern_return_t\nfixupPageAuth64(",
     "\treturn KERN_SUCCESS;\n}\n"
     "#endif /* defined(HAS_APPLE_PAC) */\n"
     "#if !defined(HAS_APPLE_PAC) && DLISA_ARM64E_FIXUPS\n"
     "/* darwin-lisa: no pointer authentication: \"signing\" is storing the plain pointer. */\n"
     "static kern_return_t\n"
     "signPointer(\n"
     "\tuint64_t         unsignedAddr,\n"
     "\t__unused void    *loc,\n"
     "\t__unused bool    addrDiv,\n"
     "\t__unused uint16_t diversity,\n"
     "\t__unused ptrauth_key key,\n"
     "\t__unused dyld_pager_t pager,\n"
     "\tuint64_t         *signedAddr)\n"
     "{\n"
     "\t*signedAddr = unsignedAddr;\n"
     "\treturn KERN_SUCCESS;\n"
     "}\n"
     "#endif /* DLISA_ARM64E_FIXUPS */\n"
     "#if defined(HAS_APPLE_PAC) || DLISA_ARM64E_FIXUPS\n"
     "\n/*\n * Apply fixups to a page used by a 64 bit process using pointer authentication.\n */\n"
     "static kern_return_t\nfixupPageAuth64("),
    # 3. route the arm64e formats
    ("\tswitch (hdr->mwli_pointer_format) {\n#if defined(HAS_APPLE_PAC)\n",
     "\tswitch (hdr->mwli_pointer_format) {\n#if defined(HAS_APPLE_PAC) || DLISA_ARM64E_FIXUPS\n"),
]

with open(F) as f:
    d = f.read()
if "--revert" in sys.argv:
    for old, new in EDITS:
        d = d.replace(new, old)
    print("dyldpager: reverted")
elif "DLISA_ARM64E_FIXUPS" in d:
    print("dyldpager: already present")
else:
    for old, new in EDITS:
        if d.count(old) != 1:
            print("dyldpager: anchor not unique/missing:", old[:60].encode()); sys.exit(1)
        d = d.replace(old, new)
    print("dyldpager: patched")
with open(F, "w") as f:
    f.write(d)
