#!/bin/sh
# T0b: CoreAnimation 26's shaders through metal2vulkan and the host turnip (drm-shim as the A642L).
#   run.sh OUT_DIR   -> OUT_DIR/cases.tsv (from HakiosMetal's logs), OUT_DIR/results.tsv
# The libraries: QuartzCore's (the OS cryptex copy; its AIR slice) first, whose every function is also tried alone, then RenderBox's
# and every other metallib of the sysroot, where the logged pipelines' other functions are.
set -eu
OUT=${1:?usage: run.sh OUT_DIR}
UNIXNU=${UNIXNU:-$HOME/Dev/mobile/userland-26/gpu-build/unixnu}
S=$HOME/Dev/mobile/userland-26/out/sysroot-26
LOGS=$HOME/Dev/mobile/research/hackios-xnu-kernel/logs
HERE=$(cd "$(dirname "$0")" && pwd)
B=$UNIXNU/build/host
mkdir -p "$OUT"
(cd "$LOGS" && python3 -I "$HERE/cases-from-logs.py" "$OUT/cases.tsv" $(ls *.log | grep -v '^[0-9]'))
{
	echo "$S/private/preboot/Cryptexes/OS/System/Library/Frameworks/QuartzCore.framework/default.metallib"
	echo "$S/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib"
	(cd "$S" && find . -name '*.metallib' -type f | sed "s|^\.|$S|" | grep -v 'QuartzCore\|RenderBox.framework' | sort)
} > "$OUT/libraries.txt"
# shellcheck disable=SC2046
MESA_SHADER_CACHE_DISABLE=true LD_PRELOAD=$B/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so FD_GPU_ID=635 \
	VK_DRIVER_FILES=$B/turnip-icd.json M2V_CACHE=$OUT/cache \
	"$UNIXNU/build/m2v-coverage/release/m2v-coverage" /usr/lib64/libvulkan.so.1 "$OUT/cases.tsv" $(cat "$OUT/libraries.txt") \
	> "$OUT/results.tsv" 2> "$OUT/results-err.txt"
rm -rf "$OUT/cache"
cut -f1,11 "$OUT/results.tsv" | sort | uniq -c
