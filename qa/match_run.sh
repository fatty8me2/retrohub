#!/bin/bash
# usage: run.sh name "spec" [frames]
cd "$(dirname "$0")/.."
export N64RUN_MUTE=1 SSB64_MUTE=1
rm -rf sv_$1; mkdir -p sv_$1
build/bin/Release/n64run.exe --rom "${ROM:?set ROM to the Smash Remix .z64 path}" --game ${GAME:-remix} --emu-dir deps/emu --save-dir sv_$1 --headless --frames ${3:-1500} --screenshot out/$1.png --dump-ram 0xA4D08,0x1F0,out/$1.vs.bin --ports 0,1,2,3 --match "$2" 2>&1 | grep "n64run\]"
