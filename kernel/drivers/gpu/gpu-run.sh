#!/usr/bin/env bash
# darwin-lisa: the GPU boots of docs/gpu-device-test.md, as host runs of tools/run-flashroot.sh (root and /private/var
# on the flash, nothing uploaded). Only the boot-args differ from a normal boot; every GPU piece is off without them.
#
#   kernel/drivers/gpu/gpu-run.sh print STEP     the boot-arg line, its length, what was dropped (no phone needed)
#   kernel/drivers/gpu/gpu-run.sh run STEP       the run (the phone in the loader, or the loader's autoboot window)
#
#   STEP  base    the known-good line, no GPU: the reference boot
#         probe   steps 1 + 2 + 3a in one boot: gpu=1 (GMU/SQE/zap firmware, the 3D core), then before launchd
#                 /usr/local/bin/gputest = hakios-metal-probe (copies, fills, kernels, triangles on the GPU) and
#                 metal_test (Metal's discovery, with metal=1 metal_plugin=HakiosMetalProbe: metal_test alone gets the
#                 device). The window server stays in software and the boot goes on to the home screen
#         ca      step 3b: CoreAnimation on Metal (metal=1: backboardd gets HakiosMetal.bundle), async rendering on
#         ca-lite step 3b with Reduce Transparency/Motion (ui_lite=1): Liquid Glass's blurs out of the way
#         ca-native step 3b with Metal's own function specialisation tried first (metal_fc_native=1)
# Environment: GEOM (the identity's geometry boot-args, appended to every step), KERNEL, XL_SECS, XL_SHOT_EVERY,
# XL_FW_RAMDISK (a firmware RAM disk that overrides the built-in GPU firmware, e.g. the pmOS set), MORE (more args).
# The line run-flashroot.sh makes must stay within 460 characters (the loader keeps 607 with its own ~120): optional
# arguments are dropped from the end of the step's list until it fits, and each drop is printed. console_open=1 is in the
# base: HakiosMetal.bundle prints to /dev/console when backboardd's stderr goes nowhere.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DL=$(cd "$HERE/../../.." && pwd)
CMD=${1:?usage: gpu-run.sh print|run STEP}
STEP=${2:?usage: gpu-run.sh print|run STEP}
MAX=460

UFS="rd=md0 ufs=2 ufs_part=super ufs_root=1 ufs_write=1 ufs_data=darwin_var var_ufs=1"
# run-flashroot.sh's own words in front (the epoch is ten digits)
FRONT="loader_return_after=1500 epoch=1794400000 sb_delay=8 hv_wdt_pet=1 bootcpu=7 debug=0x8 -v"
BASE="display=1 keystore=1 console_open=1 dlisa_unlock=1 cpus=7 cpufreq=1 cpufreq_max=2400 battery=1 wlan_stub=1"
TOUCH="touchscreen=1 touch_avdd_ldo=ldoc7 touch_chip=goodix touch_goodix_a=1"   # one group: kept or dropped whole
UM="/usr/libexec/usermanagerd:--init"
case "$STEP" in
base)      NEED="$BASE init_run=$UM init_run_secs=60"; OPT="$TOUCH|dlisa_noidle=1"; SECS=600 ;;
probe)     NEED="$BASE gpu=1 metal=1 metal_plugin=HakiosMetalProbe init_run=$UM;/usr/local/bin/gputest init_run_secs=150"
           OPT="$TOUCH|dlisa_noidle=1"; SECS=900 ;;
ca)        NEED="$BASE gpu=1 metal=1 dlisa_async=1 metal_fps=1 init_run=$UM init_run_secs=60"
           OPT="$TOUCH|dlisa_noidle=1|metal_defer=1"; SECS=1200 ;;
ca-lite)   NEED="$BASE gpu=1 metal=1 dlisa_async=1 metal_fps=1 ui_lite=1 init_run=$UM init_run_secs=60"
           OPT="$TOUCH|dlisa_noidle=1|metal_defer=1"; SECS=1200 ;;
ca-native) NEED="$BASE gpu=1 metal=1 dlisa_async=1 metal_fps=1 metal_fc_native=1 init_run=$UM init_run_secs=60"
           OPT="$TOUCH|dlisa_noidle=1"; SECS=1200 ;;
*) echo "gpu-run: unknown step $STEP" >&2; exit 1 ;;
esac
NEED="$NEED${GEOM:+ $GEOM}${MORE:+ $MORE}"
# lisa=1 (kernel/drivers/bootargs): the kernel has the touch, cpufreq and panel arguments built in, so they leave the line
case " $NEED " in
*" lisa=1 "*)
	NEED=$(sed -e 's/ cpufreq=1 cpufreq_max=2400 / /' <<<" $NEED " | sed -e 's/^ //' -e 's/ $//')
	OPT=${OPT/"$TOUCH|"/}
	OPT=${OPT/"$TOUCH"/} ;;
esac

IFS='|' read -r -a opt <<<"$OPT"     # the optional groups, the last one dropped first
dropped=()
while :; do
	EXTRA="$NEED"
	for o in "${opt[@]+"${opt[@]}"}"; do
		case " $NEED " in *" $o "*) ;; *) EXTRA="$EXTRA $o" ;; esac     # (MORE may have it already)
	done
	LINE="$FRONT $UFS $EXTRA"
	[ ${#LINE} -le $MAX ] && break
	[ ${#opt[@]} -gt 0 ] || { echo "gpu-run: $STEP needs ${#LINE} characters even without its optional arguments (at most $MAX)" >&2; exit 1; }
	dropped=("${opt[-1]}" "${dropped[@]+"${dropped[@]}"}")
	unset 'opt[-1]'
done
echo "gpu-run: $STEP: ${#LINE} of $MAX characters"
echo "gpu-run: XL_EXTRA_ARGS=\"$EXTRA\""
[ ${#dropped[@]} -eq 0 ] || { printf 'gpu-run: dropped to fit:'; printf ' [%s]' "${dropped[@]}"; echo; }
[ "$CMD" = run ] || exit 0

TAG=gpu-$STEP-$(date +%H%M)
echo "gpu-run: log research/hackios-xnu-kernel/logs/$TAG.log"
XL_UFS_ARGS="$UFS" XL_EXTRA_ARGS="$EXTRA" XL_TAG="$TAG" XL_SECS="${XL_SECS:-$SECS}" \
	XL_SHOT_EVERY="${XL_SHOT_EVERY:-20}" exec "$DL/tools/run-flashroot.sh"
