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
#
#   darwin-lisa/loader/build-ossel.sh datavol
#   the same plus data-region.patch (writes only into a region of data-regions.h, docs/data-volume.md):
#   -> out/loader-usb-lisa-datavol.hkub, out/vendor_boot-lisa-datavol.img, out/loader-datavol.efi
set -euo pipefail
M=$(cd "$(dirname "$0")/../.." && pwd)
H=$M/research/hackios-xnu-kernel
W=$M/darwin-lisa/.work/loader-ossel
KVER=2.3.4-hakios-darwin22.5-sc7280-g63fc2b46-dirty    # what the flashed payload's kernel.ver says
FLAVOR=${1:-ossel}
case "$FLAVOR" in
ossel) SRC=new PATCHES="oem-bootos.patch" ;;
datavol) SRC=datavol PATCHES="oem-bootos.patch data-region.patch" ;;
*) echo "usage: build-ossel.sh [datavol]" >&2; exit 1 ;;
esac

rm -rf "${W:?}/$SRC" && mkdir -p "$W/$SRC/xnuloader"
cp "$H"/xnuloader/{loader.c,efi.h,identity.h,build.sh} "$W/$SRC/xnuloader/"
cp "$M/darwin-lisa/loader/data-regions.h" "$W/$SRC/xnuloader/"
for p in $PATCHES; do (cd "$W/$SRC" && patch -p1 --forward <"$M/darwin-lisa/loader/$p"); done
(cd "$W/$SRC/xnuloader" && bash build.sh)
python3 -I "$H/xnuloader/mkpayload.py" -o "$W/usb-payload-$FLAVOR.bin" \
	loader.efi="$W/$SRC/xnuloader/loader.efi" --usb --kernel-version "$KVER"
python3 -I "$H/tools/mkubootimg.py" -d lisa -l "$W/$SRC/xnuloader/loader.efi" -p "$W/usb-payload-$FLAVOR.bin" \
	-c "$M/out/loader-usb-lisa-$FLAVOR.hkub"
python3 -I - "$H/out/vendor_boot-multiboot-lisa.img" "$H/out/loader-usb-lisa.hkub" "$M/out/loader-usb-lisa-$FLAVOR.hkub" \
	"$M/out/vendor_boot-lisa-$FLAVOR.img" <<'EOF'
import struct, sys
src, old_hkub, new_hkub, out = sys.argv[1:]
d = bytearray(open(src, 'rb').read())
assert d[:8] == b'VNDRBOOT' and struct.unpack_from('<I', d, 8)[0] == 3
page, vr = struct.unpack_from('<I', d, 12)[0], struct.unpack_from('<I', d, 24)[0]
at = (struct.unpack_from('<I', d, 28 + 2048 + 4 + 16)[0] + page - 1) // page * page
old, new = open(old_hkub, 'rb').read(), open(new_hkub, 'rb').read()
assert d[at:at + vr] == old, 'the vendor ramdisk is not the HKUB it should be'
if len(new) == vr:
    d[at:at + vr] = new
    open(out, 'wb').write(d)
    print(f'{out}: vendor ramdisk replaced in place ({vr} bytes at {at:#x}); header and DTB untouched')
    sys.exit(0)
# another size (data-region.patch makes the loader a page longer): the v3 layout again, header | ramdisk | DTB, each
# padded to a page. Only vendor_ramdisk_size changes in the header; the DTB bytes are the same, one page further on.
pad = lambda b: b + bytes(-len(b) % page)
hs, dtb_size = struct.unpack_from('<II', d, 2096)
assert at == (hs + page - 1) // page * page and len(d) == at + (vr + page - 1) // page * page + len(pad(b'x' * dtb_size))
dtb_at = at + (vr + page - 1) // page * page
dtb = bytes(d[dtb_at:dtb_at + dtb_size])
hdr = bytearray(d[:at])
struct.pack_into('<I', hdr, 24, len(new))
img = bytes(hdr) + pad(new) + pad(dtb)
open(out, 'wb').write(img)
print(f'{out}: repacked: vendor ramdisk {vr} -> {len(new)} bytes, DTB ({dtb_size} bytes, same) at {at + len(pad(new)):#x}; '
      f'header changed only in vendor_ramdisk_size')
EOF
cp "$W/$SRC/xnuloader/loader.efi" "$M/out/loader-$FLAVOR.efi"
sha256sum "$M/out/loader-usb-lisa-$FLAVOR.hkub" "$M/out/vendor_boot-lisa-$FLAVOR.img" "$M/out/loader-$FLAVOR.efi"
