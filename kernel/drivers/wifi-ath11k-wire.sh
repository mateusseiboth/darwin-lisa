#!/bin/bash
# darwin-lisa: the two deferred Wi-Fi pieces of README-wifi.md (1 and 3), so that step 15 (the firmware's WLFW
# exchange) and the ath11k driver (scan, join) are built into the iOS 26 kernel:
#   1. ath11k itself: HakiOS's tools/mk-ath11k.sh (Linux 7.0.14's ath11k on FreeBSD's LinuxKPI + HakiOS wifi/lx),
#      -> research/hackios-xnu-kernel/xnu-deps/build/ath11k/out/ath11k.o, linked into the kernel by
#      .work/rebuild26.sh (XNU_EXTRA_LD_ARCHIVES, only when the object is there).
#   2. hakios.wlfw_ath11k.h: ath11k's QMI message tables (tools/mk-wlfw.sh), written next to hakios.wlfw.c in
#      bsd/dev/ so its __has_include picks it up; hakios.wlfw.c is touched so make compiles it again.
# Run in docker as root (the xnu-work tree is root-owned; mk-ath11k.sh fetches ~400 MB the first time):
#   docker run --rm -v ~/Dev/mobile:/home/mateusseiboth/Dev/mobile -e HOME=/home/mateusseiboth \
#     darwin-lisa/unixnu-toolchain:latest bash /home/mateusseiboth/Dev/mobile/darwin-lisa/kernel/drivers/wifi-ath11k-wire.sh
# then .work/run-rebuild.sh. Idempotent.
set -euo pipefail
HK=/home/mateusseiboth/Dev/mobile/research/hackios-xnu-kernel
T=/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work
LX=$HK/xnu-deps/build/ath11k/out/ath11k.o

[ -f "$LX" ] || (cd "$HK" && bash tools/mk-ath11k.sh)
echo "  [ok]   $LX"

# mk-wlfw.sh writes into $XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT: point that at bsd/dev
FAKE=$(mktemp -d)
mkdir -p "$FAKE/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd"
ln -s "$T/bsd/dev" "$FAKE/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT"
(cd "$HK" && XNU_SRC=$FAKE bash tools/mk-wlfw.sh)
rm -rf "$FAKE"
touch "$T/bsd/dev/hakios.wlfw.c"
echo "  [ok]   $T/bsd/dev/hakios.wlfw_ath11k.h"

# lisa has no bd_m20gl.elf of its own here, but pmOS's board-2.bin (the container ath11k asks for first, and picks
# the board's entry from by its ids) is in /fw: serve it under its own name too.
python3 - "$T/bsd/dev/hakios.wifi.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '{ { "board.bin", "bd_m20gl.elf" }, { "regdb.bin", "regdb_xiaomi.bin" } };'
new = '{ { "board.bin", "bd_m20gl.elf" }, { "regdb.bin", "regdb_xiaomi.bin" },\n\t    { "board-2.bin", "board-2.bin" } /* darwin-lisa: pmOS\'s container (lisa) */ };'
if new in s:
    print("  [skip] hakios.wifi.c serves board-2.bin")
else:
    assert s.count(old) == 1, "anchor"
    open(p, "w").write(s.replace(old, new))
    print("  [ok]   hakios.wifi.c serves board-2.bin")
PY

# The link: .work/rebuild26.sh passes ath11k.o to the UniXNU build as XNU_EXTRA_LD_ARCHIVES when it exists (the build
# sets LD_KERNEL_ARCHIVES on make's command line, so makedefs cannot add it), and relinks when the object is newer.
touch "$LX"
echo "### WIFI ATH11K WIRE DONE"
