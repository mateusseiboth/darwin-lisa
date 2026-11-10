#!/usr/bin/env bash
# darwin-lisa: build the xnuloader with oem-bootos.patch WITHOUT touching research/hackios-xnu-kernel/xnuloader:
# the sources are copied to darwin-lisa/.work/loader-ossel/new, patched and built there (clang + lld-link, as
# xnuloader/build.sh). Then the same USB payload and HKUB container as the flashed ones (recipe checked byte for byte
# against research/hackios-xnu-kernel/out/loader-usb-lisa.hkub), and a vendor_boot image that differs from
# out/vendor_boot-multiboot-lisa.img ONLY inside the vendor ramdisk (same header, same DTB bytes).
#
#   darwin-lisa/loader/build-ossel.sh
#   -> out/loader-usb-lisa-ossel.hkub   (for U-Boot's "HakiOS over fastboot": RAM only)
#   -> out/vendor_boot-lisa-ossel.img   (for vendor_boot_a: a WRITE, only with the user's approval)
set -euo pipefail
M=$(cd "$(dirname "$0")/../.." && pwd)
H=$M/research/hackios-xnu-kernel
W=$M/darwin-lisa/.work/loader-ossel
KVER=2.3.4-hakios-darwin22.5-sc7280-g63fc2b46-dirty    # what the flashed payload's kernel.ver says

rm -rf "$W/new" && mkdir -p "$W/new/xnuloader"
cp "$H"/xnuloader/{loader.c,efi.h,identity.h,build.sh} "$W/new/xnuloader/"
(cd "$W/new" && patch -p1 --forward <"$M/darwin-lisa/loader/oem-bootos.patch")
(cd "$W/new/xnuloader" && bash build.sh)
python3 -I "$H/xnuloader/mkpayload.py" -o "$W/usb-payload-ossel.bin" \
	loader.efi="$W/new/xnuloader/loader.efi" --usb --kernel-version "$KVER"
python3 -I "$H/tools/mkubootimg.py" -d lisa -l "$W/new/xnuloader/loader.efi" -p "$W/usb-payload-ossel.bin" \
	-c "$M/out/loader-usb-lisa-ossel.hkub"
python3 -I - "$H/out/vendor_boot-multiboot-lisa.img" "$H/out/loader-usb-lisa.hkub" "$M/out/loader-usb-lisa-ossel.hkub" \
	"$M/out/vendor_boot-lisa-ossel.img" <<'EOF'
import struct, sys
src, old_hkub, new_hkub, out = sys.argv[1:]
d = bytearray(open(src, 'rb').read())
assert d[:8] == b'VNDRBOOT' and struct.unpack_from('<I', d, 8)[0] == 3
page, vr = struct.unpack_from('<I', d, 12)[0], struct.unpack_from('<I', d, 24)[0]
at = (struct.unpack_from('<I', d, 28 + 2048 + 4 + 16)[0] + page - 1) // page * page
old, new = open(old_hkub, 'rb').read(), open(new_hkub, 'rb').read()
assert d[at:at + vr] == old, 'the vendor ramdisk is not the HKUB it should be'
assert len(new) == vr, 'the new container is another size: repack with mkbootimg instead'
d[at:at + vr] = new
open(out, 'wb').write(d)
print(f'{out}: vendor ramdisk replaced in place ({vr} bytes at {at:#x}); header and DTB untouched')
EOF
cp "$W/new/xnuloader/loader.efi" "$M/out/loader-ossel.efi"
sha256sum "$M/out/loader-usb-lisa-ossel.hkub" "$M/out/vendor_boot-lisa-ossel.img" "$M/out/loader-ossel.efi"
