#!/bin/sh
# Build DLisaAudio.driver (dlisa-audio.c, the speakers' AudioServerPlugIn for audiomxd) into OUT (default
# userland/audio/out/DLisaAudio.driver): the host tests first, then clang in the toolchain image with the iOS 16.5 SDK
# (as libdlisa), -bundle, ad-hoc signed; arm64e-mark.py labels it arm64e (audiomxd is arm64e: dlopen takes only a
# bundle of the process's own architecture) and re-hashes the signature. Installed into a root by the root build
# (build-root-audio.sh), never into the shared sysroot.
set -eu
M=/home/mateusseiboth/Dev/mobile
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/out/DLisaAudio.driver}
T=$(mktemp -d "${TMPDIR:-/tmp}/dlisa-audio.XXXXXX")
trap 'rm -rf "$T"' EXIT
cc -std=c11 -Wall -Wextra -Werror -o "$T/logic_test" "$HERE/test/dlisa_audio_logic_test.c" -lm && "$T/logic_test"
docker run --rm -v $M:/work darwin-lisa/unixnu-toolchain:latest sh -c '
	S=/work/research/hackios-xnu-kernel/xnu-deps/theos/sdks-git/iPhoneOS16.5.sdk
	clang -target arm64-apple-ios16.0 -isysroot $S -O2 -Wall -Wextra -Werror -Wno-unused-parameter -bundle \
	    --ld-path=/work/research/hackios-xnu-kernel/xnu-deps/host/bin/ld64-lld -framework CoreFoundation \
	    -Wl,-fixup_chains -Wl,-adhoc_codesign -o /tmp/DLisaAudio /work/darwin-lisa/userland/audio/dlisa-audio.c >&2 &&
	cat /tmp/DLisaAudio' > "$T/DLisaAudio"
python3 "$HERE/../inject/arm64e-mark.py" "$T/DLisaAudio"
mkdir -p "$OUT"
install -m 755 "$T/DLisaAudio" "$OUT/DLisaAudio"
install -m 644 "$HERE/Info.plist" "$OUT/Info.plist"
sha256sum "$OUT/DLisaAudio" "$OUT/Info.plist"
