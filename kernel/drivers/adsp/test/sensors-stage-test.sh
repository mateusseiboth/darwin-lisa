#!/bin/sh
# Host test of fw-sensors-stage.py on a made-up vendor tree, registry and persist dump (no proprietary file needed):
# stage, check, the header twice (the same bytes), what each source puts where and in which order of preference,
# and the archive read back through the kernel's own file server (sensors-archive-test, built by run-fastrpc-tests.sh).
#   sh sensors-stage-test.sh PATH/TO/sensors-archive-test
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
STAGE="$HERE/../fw-sensors-stage.py"
ARCHIVE_TEST=$1
W=$(mktemp -d "${TMPDIR:-/tmp}/sensors-stage-test.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "sensors-stage-test: FAIL: $*"; exit 1; }

mkdir -p "$W/vendor/etc/sensors/config" "$W/vendor/etc/sensors/proto" "$W/registry" "$W/socinfo" \
    "$W/persist/sensors/registry/registry"
printf '{"config":{"hw_platform":["QRD"],"soc_id":["475"]}}\n' > "$W/vendor/etc/sensors/config/sm7350_icm4x6xx_0.json"
printf '{"config":{}}\n' > "$W/vendor/etc/sensors/config/sns_tilt.json"
printf 'version=6\nfile=output=/mnt/vendor/persist/sensors/registry/registry\n' > "$W/vendor/etc/sensors/sns_reg_config"
printf 'not staged\n' > "$W/vendor/etc/sensors/proto/sns_accel.proto"
printf '{"icm4x6xx_0":{"owner":"icm4x6xx"}}' > "$W/registry/icm4x6xx_0"
: > "$W/registry/sensors_registry"
printf 'QRD\n' > "$W/socinfo/hw_platform"
printf '475\n' > "$W/socinfo/soc_id"
printf '{"from":"the phone"}' > "$W/persist/sensors/registry/registry/icm4x6xx_0"
printf 'version=6\n' > "$W/persist/sensors/registry/sns_reg_version"

# 1. the registry from a directory (the pmOS firmware package's kind)
python3 -I "$STAGE" stage --vendor "$W/vendor" --registry "$W/registry" --socinfo "$W/socinfo" --out "$W/out" > "$W/log1"
R="$W/out/root"
test -f "$R/vendor/etc/sensors/config/sm7350_icm4x6xx_0.json" || fail "the configs are not staged"
test -f "$R/vendor/etc/sensors/sns_reg_config" || fail "sns_reg_config is not staged"
test ! -e "$R/vendor/etc/sensors/proto" || fail "the .proto files are staged (the DSP never reads them)"
test -f "$R/mnt/vendor/persist/sensors/registry/registry/icm4x6xx_0" || fail "the registry is not staged"
test -f "$R/sys/devices/soc0/soc_id" || fail "socinfo is not staged"
grep -q "registry: $W/registry" "$W/out/SOURCE" || fail "SOURCE does not say where the registry came from"
python3 -I "$STAGE" check "$W/out" > /dev/null || fail "check refuses what stage made"
"$ARCHIVE_TEST" "$W/out/sensors.fs" "$R" || fail "the archive does not read back as the tree"

# 2. a persist dump wins over --registry
python3 -I "$STAGE" stage --vendor "$W/vendor" --registry "$W/registry" --persist "$W/persist/sensors" \
    --socinfo "$W/socinfo" --out "$W/out" > "$W/log2"
grep -q 'the phone' "$R/mnt/vendor/persist/sensors/registry/registry/icm4x6xx_0" || fail "the persist dump did not win"
test -f "$R/mnt/vendor/persist/sensors/registry/sns_reg_version" || fail "the persist dump's sns_reg_version is missing"
test ! -e "$R/mnt/vendor/persist/sensors/registry/registry/sensors_registry" || fail "a file of the other registry is left"
"$ARCHIVE_TEST" "$W/out/sensors.fs" "$R" || fail "the archive with persist does not read back"

# 3. the header: .incbin of the archive, the same bytes twice
python3 -I "$STAGE" header "$W/out" "$W/h1.h" > /dev/null
python3 -I "$STAGE" header "$W/out" "$W/h2.h" > /dev/null
cmp -s "$W/h1.h" "$W/h2.h" || fail "the header differs between two runs"
grep -q "incbin" "$W/h1.h" || fail "no .incbin in the header"
grep -q "DLISA_SENSORS_FS_SIZE $(wc -c < "$W/out/sensors.fs" | tr -d ' ')" "$W/h1.h" || fail "the header's size is wrong"
cat > "$W/use.c" <<EOF
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "h1.h"
int main(void) { return !(memcmp(dlisa_sensors_fs, "DLSNSFS1", 8) == 0 && DLISA_SENSORS_FS_SIZE > 16); }
EOF
cc -std=gnu11 -o "$W/use" "$W/use.c" -I"$W" && "$W/use" || fail "the header does not link the archive in"

# 4. refusals: a missing vendor tree, a non-absolute archive path can't happen, an empty registry is allowed
if python3 -I "$STAGE" stage --vendor "$W/nonexistent" --registry "$W/registry" --socinfo "$W/socinfo" --out "$W/bad" \
    > /dev/null 2>&1; then
	fail "a missing vendor tree is accepted"
fi
echo "sensors-stage-test: ok"
