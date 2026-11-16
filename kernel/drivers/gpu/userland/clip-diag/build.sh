#!/bin/sh
# The plug-in with the clip diagnostics (session 15), built apart from the shared patch series: a copy of ../ (the
# committed patches and build-hakios-metal.sh) in a temporary directory, with the two patches here added last --
# hakios-metal-z-clip-diag.patch (core) after every hakios-metal-*.patch, HakiosMetalCA-clip-diag.patch (plug-in)
# after HakiosMetalCA-ubwc-rt.patch -- and the core patch left out of the compiled-stage cache key (it changes no
# compiled stage, so the cache the phone already has stays good). ../ itself is not touched, so the other builds
# do not pick these up.
#
#   DEST=DIR DEST_BIN=DIR sh build.sh      (as build-hakios-metal.sh: PLUGIN=ca MESA=1 are set here)
#
# What the patches add (all inert without their boot-arg):
#   metal_trace=1     each kind of render pass once ("trace: pass a0 ... a1 ... a2 ...": size, format, storage S/P/M,
#                     load/store), each pipeline's vertex constants and its colour attachments 1 and 2 ("trace: ...
#                     vertex constants"), and the core's counts of fetches of a missing target given attachment 0 and of
#                     constant ranges pushed as zeros for an unbound buffer ("trace: so far ...")
#   metal_ml_clear=1  a memoryless colour attachment past 0 that begins DontCare is cleared to zero (an experiment)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
U="$STAGE/kernel/drivers/gpu/userland"
mkdir -p "$U" "$STAGE/userland/inject"
cp "$HERE"/../*.patch "$HERE"/../build-hakios-metal.sh "$U/"
cp "$HERE/hakios-metal-z-clip-diag.patch" "$HERE/HakiosMetalCA-clip-diag.patch" "$U/"
cp "$HERE/../../../../../userland/inject/arm64e-mark.py" "$STAGE/userland/inject/"
python3 -I - "$U/build-hakios-metal.sh" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = '       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-ubwc-rt.patch"\n'
assert s.count(a) == 1
s = s.replace(a, a + '       patch -s -d "$WORK" -p1 < "$HERE/HakiosMetalCA-clip-diag.patch"\n')
b = '"$HERE"/hakios-metal-*.patch "$H"/hakios-metal/include'
assert s.count(b) == 1
s = s.replace(b, '$(ls "$HERE"/hakios-metal-*.patch | grep -v z-clip-diag) "$H"/hakios-metal/include')
open(p, 'w').write(s)
PY
PLUGIN=ca MESA=1 sh "$U/build-hakios-metal.sh"
