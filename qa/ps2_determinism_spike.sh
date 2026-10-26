#!/bin/bash
export RETRORUN_MUTE=1 RETRORUN_HASHLOG=60
G="$1"; ROM="$2"
for i in a b; do rm -f scratch/${G}_$i.log; ( qa/ps2_run_spike.sh "$ROM" --frames 18000 --fuzz-input --log scratch/${G}_$i.log >/dev/null 2>&1 ) & done; wait
echo done $G > scratch/${G}.done
