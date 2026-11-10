#!/bin/sh
# QuartzCore's shader library where CoreAnimation 26 looks for it first.
#
# The MetalContext of iOS 26's QuartzCore (sub_1841545e8 in 23F84) asks
#   [[NSBundle bundleForClass:...] URLForResource:@"default" withExtension:@"metallib"]
# and asserts `metallib_url != nil` (ogl-metal.mm) when that is nil. The bundle is
# /System/Library/Frameworks/QuartzCore.framework, which in the rootfs holds only Info.plist and _CodeSignature: the
# library itself ships in the OS cryptex (/System/Cryptexes/OS/System/Library/Frameworks/QuartzCore.framework/
# default.metallib, here at private/preboot/Cryptexes/OS/...). CoreFoundation knows the cryptex paths, so on the phone the
# lookup probably finds it there; this step makes it certain by putting a copy beside Info.plist.
#
#   ca-metallib-stage.sh            say where the library is and whether the rootfs copy is there (changes nothing)
#   ca-metallib-stage.sh apply      a symbolic link to the cryptex copy (no blocks: the root image has ~2200 to spare),
#                                   then rebuild the image. COPY=1: a copy instead (8.9 MB, ~1900 blocks compressed)
#   ca-metallib-stage.sh remove     take the copy out again
#   SYSROOT=DIR                     another sysroot (default ~/Dev/mobile/userland-26/out/sysroot-26)
set -eu
SYSROOT=${SYSROOT:-/home/mateusseiboth/Dev/mobile/userland-26/out/sysroot-26}
REL=System/Library/Frameworks/QuartzCore.framework
SRC=$SYSROOT/private/preboot/Cryptexes/OS/$REL/default.metallib
DST=$SYSROOT/$REL/default.metallib
[ -f "$SRC" ] || { echo "ca-metallib-stage: no $SRC (the OS cryptex is not laid out in this sysroot)"; exit 1; }
[ -f "$SYSROOT/$REL/Info.plist" ] || { echo "ca-metallib-stage: no $SYSROOT/$REL/Info.plist"; exit 1; }
case "${1:-check}" in
check)
	echo "cryptex copy: $SRC ($(stat -c %s "$SRC") bytes)"
	if [ -L "$DST" ]; then
		echo "rootfs link:  $DST -> $(readlink "$DST")"
	elif [ -f "$DST" ]; then
		cmp -s "$SRC" "$DST" && echo "rootfs copy:  $DST, the same file" || echo "rootfs copy:  $DST, DIFFERENT from the cryptex one"
	else
		echo "rootfs copy:  none (\`$0 apply\` puts one there)"
	fi ;;
apply)
	rm -f "$DST"
	if [ "${COPY:-0}" = 1 ]; then
		install -m 644 "$SRC" "$DST"
	else
		# the path as the phone sees it: the cryptex is laid out at /private/preboot/Cryptexes/OS in the root image
		ln -s "/private/preboot/Cryptexes/OS/$REL/default.metallib" "$DST"
	fi
	echo "ca-metallib-stage: $DST; now rebuild the image (cd ~/Dev/mobile/userland-26 && LEAN=1 bash scripts/mkimage-trim.sh)" ;;
remove)
	rm -f "$DST"
	echo "ca-metallib-stage: $DST removed" ;;
*)
	echo "usage: $0 [check|apply|remove]"; exit 1 ;;
esac
