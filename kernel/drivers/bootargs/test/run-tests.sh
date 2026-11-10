#!/bin/sh
# Host test of ../lisa-defaults-wire.py: the pristine pexpert/gen/bootargs.c (the UniXNU feed's) is wired on a copy
# twice (the second run must be all [skip]), then compiled with stand-in headers and bootargs-defaults-test.c.
#   sh kernel/drivers/bootargs/test/run-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${BOOTARGS_C:-/home/mateusseiboth/Dev/mobile/research/UniXNU/out/feeds/xnu-3199e8bc/pexpert/gen/bootargs.c}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

mkdir -p "$W/tree/pexpert/gen" "$W/inc/pexpert"
cp "$SRC" "$W/tree/pexpert/gen/bootargs.c"
XNU_WORK="$W/tree" python3 -I "$HERE/../lisa-defaults-wire.py" >"$W/first.log"
XNU_WORK="$W/tree" python3 -I "$HERE/../lisa-defaults-wire.py" >"$W/second.log"
grep -q '\[ok\]' "$W/first.log" || { cat "$W/first.log"; echo "run-tests: the first run applied nothing"; exit 1; }
if grep '\[' "$W/second.log" | grep -qv '\[skip\]'; then
	cat "$W/second.log"
	echo "run-tests: the second run was not all [skip]"
	exit 1
fi

cat >"$W/inc/pexpert/pexpert.h" <<'EOF'
#include <stdint.h>
#include <string.h>
typedef int boolean_t;
#define TRUE 1
#define FALSE 0
char *PE_boot_args(void);
boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_len);
boolean_t PE_parse_boot_arg_str(const char *arg_string, char *arg_ptr, int size);
boolean_t PE_boot_arg_uint64_eq(const char *arg_string, uint64_t value);
boolean_t PE_get_default(const char *property_name, void *property_ptr, unsigned int max_property);
boolean_t PE_imgsrc_mount_supported(void);
EOF
cat >"$W/inc/pexpert/device_tree.h" <<'EOF'
#include <stdbool.h>
typedef void *DTEntry;
#define kSuccess 0
#define SecureDTLookupEntry(parent, path, entry) (-1)
#define SecureDTGetProperty(entry, name, data, size) (-1)
EOF
cc -std=gnu11 -w -Wno-incompatible-pointer-types -I"$W/inc" -o "$W/test" "$HERE/bootargs-defaults-test.c" "$W/tree/pexpert/gen/bootargs.c"
"$W/test"
