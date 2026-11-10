#!/bin/sh
# Syntax-only compile of the ADSP/battmgr/sensors/FastRPC kernel files with the EXACT flags of the last xnu-12377 build
# (the build record XNU writes next to the objects: bsd/DEVELOPMENT/.CFLAGS, iokit/DEVELOPMENT/.CXXFLAGS), in the
# toolchain image. Nothing is written to the tree or the build directory: -fsyntax-only, run from the object dir
# because the flags use it as the include path for meta_features.h. The record is the make line unquoted
# (-DCONFIG_NMBCLUSTERS=((1024 * 256) / MCLBYTES)), so the words are re-joined here until their brackets balance.
# Usage: sh check-syntax.sh   (prints the errors and warnings per file, then a summary)
set -eu
M=/home/mateusseiboth/Dev/mobile
OBJ=$M/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837
HERE=$M/darwin-lisa/kernel/drivers
WORK=$(mktemp -d "${TMPDIR:-/tmp}/adsp-syntax.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
python3 -I - "$OBJ" "$HERE" "$WORK/run.sh" <<'EOF'
import shlex, sys
obj, here, out = sys.argv[1:4]
def words(path):
    raw = open(path, 'rb').read().split(b'\0')[0].decode().split()
    joined, cur = [], ''
    for w in raw:
        cur = w if not cur else cur + ' ' + w
        if cur.count('(') == cur.count(')'):
            joined.append(cur); cur = ''
    return joined
jobs = [('bsd/DEVELOPMENT', '.CFLAGS', 'c', 'adsp/hakios.adsp.c'), ('bsd/DEVELOPMENT', '.CFLAGS', 'c', 'adsp/dlisa.ssc.c'),
        ('bsd/DEVELOPMENT', '.CFLAGS', 'c', 'adsp/dlisa.fastrpc.c'),
        ('bsd/DEVELOPMENT', '.CFLAGS', 'c', 'battery/dlisa.battery.c'),
        ('iokit/DEVELOPMENT', '.CXXFLAGS', 'c++', 'battery/hakios.battery.cpp')]
with open(out, 'w') as f:
    f.write('fails=0\n')
    for d, flags, lang, src in jobs:
        cmd = words(f'{obj}/{d}/{flags}') + ['-fsyntax-only', f'-x{lang}', f'{here}/{src}']
        f.write(f'cd {shlex.quote(obj + "/" + d)}\n')
        f.write(f'out=$({" ".join(shlex.quote(w) for w in cmd)} 2>&1)\n')
        f.write('e=$(printf "%s\\n" "$out" | grep -c " error: " || true)\n')
        f.write('w=$(printf "%s\\n" "$out" | grep -c " warning: " || true)\n')
        f.write('printf "%s\\n" "$out" | grep -E " (error|warning): " | grep -v "multiple candidates for header" | head -40\n')
        f.write(f'echo "== {src}: $e errors, $w warnings"\n')
        f.write('[ "$e" = 0 ] || fails=$((fails + 1))\n')
    f.write('echo "== files with errors: $fails"\n')
EOF
docker run --rm -v $M:$M -v "$WORK:$WORK" -e HOME=$M darwin-lisa/unixnu-toolchain:latest sh "$WORK/run.sh"
