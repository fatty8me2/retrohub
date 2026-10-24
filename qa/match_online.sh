#!/bin/bash
# usage: online.sh tag "spec" frames
cd "$(dirname "$0")/.."
: "${ROM:?set ROM to the Smash Remix .z64 path}"
export N64RUN_MUTE=1 SSB64_MUTE=1
ROOM=$(python -c "import random;print(\"\".join(random.choice(\"ABCDEFGHJKMNPQRSTUVWXYZ23456789\") for _ in range(6)))")-1
for s in 0 1; do
  rm -rf sv_on$s; mkdir -p sv_on$s
  build/bin/Release/n64run.exe --rom "$ROM" --game remix --emu-dir deps/emu --save-dir sv_on$s --headless --frames $3 --screenshot out/$1_$s.png --dump-ram 0xA4D08,0x1F0,out/$1_$s.vs.bin --net "${NET:-ws://127.0.0.1:8761}" --room $ROOM --players 2 --ports 0,1,-1,-1 --slot $s --match "$2" > out/$1_$s.log 2>&1 &
done
wait
