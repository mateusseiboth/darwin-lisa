#!/bin/bash
# gpu-endrun.sh OUT: oem stop, wait (90 s max) for the kernel's "run is over"/"only quiesced" line, save the log to OUT, print the verdict
OUT=${1:?}
timeout 10 fastboot oem stop >/dev/null 2>&1
for i in $(seq 1 30); do
	timeout 5 fastboot oem log >/dev/null 2>&1
	timeout 15 fastboot get_staged "$OUT.raw" >/dev/null 2>&1
	tr -d '\000' < "$OUT.raw" > "$OUT"
	LC_ALL=C grep -a -q "run is over\|only quiesced\|=== PANIC" "$OUT" && break
	sleep 3
done
echo "no answer lines: $(LC_ALL=C grep -a -c 'no answer' "$OUT")"
LC_ALL=C grep -a -E "about to end|way back is done|only quiesced|run is over|=== PANIC" "$OUT" | tail -4 | cut -c1-160
LC_ALL=C grep -a "cx_gdscr" "$OUT" | tail -1 | cut -c1-120
