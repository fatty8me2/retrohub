#!/bin/bash
# macOS twin of qa/gb_gba_synctest.ps1: Game Boy / GBA rollback SyncTest on generated homebrew ROMs (qa/make_gb_gba_roms.py): no
# commercial game needed. Muted, headless (no window), offline. Per core: the frames run, the .srm is written, the SyncTest (roll back
# -Depth frames every frame, re-simulate, compare) passes. Cores that are not installed are skipped.
# Usage: qa/gb_gba_synctest.sh [build-dir] [frames] [depth]     (defaults: build-mac 900 8)
# Cores: <build>/bin/cores/ (scripts/fetch-cores.sh stages the buildbot cores, the build stages sameboy_rb).
# Exit 0 pass, 1 failure, 77 nothing ran (ctest SKIP_RETURN_CODE).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-build-mac}"; FRAMES="${2:-900}"; DEPTH="${3:-8}"
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
EXE="$BUILD/bin/retrorun"; CORES="$BUILD/bin/cores"
ROMS="$ROOT/deps/testroms"; OUT="$ROOT/qa/out/gb_gba"
mkdir -p "$OUT" "$ROMS"
python3 -I "$ROOT/qa/make_gb_gba_roms.py" "$ROMS" >/dev/null || { echo "ROM generation failed"; exit 1; }
TO=""; command -v timeout >/dev/null && TO="timeout 300"   # stock macOS has no timeout(1); coreutils provides it
export RETRORUN_MUTE=1 SSB64_MUTE=1 RETROHUB_START_HINT=0 RETROHUB_QA=1

# name|core|rom|system|srm
CASES='gambatte-gb|gambatte_libretro.dylib|gb_test_nortc.gb|gb|gb_test_nortc.srm
sameboy-gb|sameboy_libretro.dylib|gb_test.gb|gb|gb_test.srm
sameboy_rb-gb|sameboy_rb_libretro.dylib|gb_test.gb|gb|gb_test.srm
mgba-gba|mgba_libretro.dylib|gba_test.gba|gba|'
fail=0; ran=0
while IFS='|' read -r n core rom sys srm; do
    if [ ! -f "$CORES/$core" ] || [ ! -x "$EXE" ]; then echo "SKIP $n (core or retrorun missing)"; continue; fi
    for mode in run synctest; do
        sv="$OUT/sv_${n}_$mode"; rm -rf "$sv"; mkdir -p "$sv"
        log="$OUT/${n}_$mode.log"
        args=(--core "$CORES/$core" --rom "$ROMS/$rom" --system "$sys" --save-dir "$sv" --state-dir "$sv"
              --headless --frames "$FRAMES" --log "$log")
        [ "$mode" = synctest ] && args+=(--synctest "$DEPTH")
        $TO "$EXE" "${args[@]}" >/dev/null 2>&1; rc=$?
        srmok=1
        if [ -n "$srm" ]; then [ -s "$sv/$srm" ] || srmok=0; fi
        sum="$(grep -E 'SYNCTEST|frames' "$log" 2>/dev/null | tail -n 2 | tr '\n' '|')"
        echo "$n $mode: exit $rc srm_ok=$srmok  $sum"
        ran=$((ran+1))
        if [ "$rc" != 0 ] || [ "$srmok" = 0 ]; then fail=$((fail+1)); fi
    done
done <<EOF2
$CASES
EOF2
if [ "$fail" != 0 ]; then echo "GB/GBA SYNCTEST FAILED ($fail)"; exit 1; fi
if [ "$ran" = 0 ]; then echo "GB/GBA SYNCTEST SKIPPED (no cores or retrorun)"; exit 77; fi
echo "GB/GBA SYNCTEST PASS ($ran runs)"
