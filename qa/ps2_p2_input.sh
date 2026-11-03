#!/bin/bash
# PS2 online: proof that players 2-4 REALLY reach the game (not just that the core reads their port). Runs qa/ps2_online.sh REPS times with a fuzzed pad on
# port 0 only and REPS times with pads on every port, and compares the EE RAM check at sample 600. The adaptive input delay makes single runs vary, so the
# two SETS of hashes must be disjoint (fixed build) - with RETRORUN_PS2_NOPLUG=1 (v1.31.6: LRPS2 leaves console port 2 / tap slots unplugged) they overlap.
# usage: [PLAYERS=2..4] [REPS=3] qa/ps2_p2_input.sh <iso> <frames> [port]    (repo root; same scratch layout as ps2_online.sh)
ISO="$1"; F="${2:-1800}"; PORT="${3:-11400}"; N="${PLAYERS:-2}"; ALL=$(( (1<<N) - 1 )); R="${REPS:-3}"
sigs(){ for i in $(seq $R); do rm -f scratch/net_p2_$1_*.log; RETRORUN_FUZZ_PORTS=$1 bash qa/ps2_online.sh p2_$1 "$ISO" $F direct $PORT >/dev/null 2>&1; grep -h "sync check sample 600 " scratch/net_p2_$1_$((N-1)).log | awk '{print $6}'; done | sort -u | tr '\n' ' '; }
A=$(sigs 1); B=$(sigs $ALL)
echo "pad on port 0 only: $A"; echo "pads on ports 0..$((N-1)): $B"
for x in $A; do for y in $B; do [ "$x" = "$y" ] && { echo "FAIL: players 2+ are invisible to the game (same EE RAM with and without their input)"; exit 1; }; done; done
echo "PASS: extra players change the game"
