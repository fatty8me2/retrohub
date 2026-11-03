#!/bin/bash
# Headless room-page shots (host 1/4 players, guest, long titles, narrow window) -> qa/shots/room_after/*.png
cd "$(dirname "$0")/.."
out=qa/shots/room_after; mkdir -p $out
export RETROHUB_NO_SHELL=1 SSB64_MUTE=1 RETRORUN_MUTE=1 N64RUN_MUTE=1 RETROHUB_QA=1
shot(){ n=$1; sz=$2; shift 2; (export HUB_SHOT_FAKE_ROOM=1 HUB_SHOT_SIZE=$sz HUB_SCREENSHOT="$(pwd -W)/$out/$n.bmp" HUB_SHOT_SECS=3 "$@"; build/bin/Release/RetroHub.exe >/dev/null); }
for sz in 1280x800 1600x1000; do
  shot host4_$sz $sz HUB_SHOT_FAKE_LIB=6 HUB_SHOT_ROOM_GAME=crashbash HUB_SHOT_ROOM_N=4 HUB_SHOT_READY=1
  shot host1_$sz $sz HUB_SHOT_FAKE_LIB=6 HUB_SHOT_ROOM_GAME=crashbash HUB_SHOT_ROOM_N=1
  shot guest_$sz $sz HUB_SHOT_FAKE_LIB=6 HUB_SHOT_ROOM_GAME=crashbash HUB_SHOT_ROOM_N=3 HUB_SHOT_HOSTCHANGE=other
done
shot long_zelda_1280x800 1280x800 HUB_SHOT_FAKE_LIB=6 HUB_SHOT_ROOM_GAME=crashbash HUB_SHOT_ROOM_N=2 HUB_SHOT_ROOM_TITLE="The Legend of Zelda: Twilight Princess" HUB_SHOT_ROOM_HACK=1
shot long_gauntlet_1100x700 1100x700 HUB_SHOT_FAKE_LIB=6 HUB_SHOT_ROOM_GAME=crashbash HUB_SHOT_ROOM_N=2 HUB_SHOT_ROOM_TITLE="Gauntlet - Dark Legacy" HUB_SHOT_ROOM_HACK=1
shot long_narrow_900x700 900x700 HUB_SHOT_FAKE_LIB=6 HUB_SHOT_ROOM_GAME=crashbash HUB_SHOT_ROOM_N=2 HUB_SHOT_ROOM_TITLE="The Legend of Zelda: Twilight Princess HD Remaster" HUB_SHOT_ROOM_HACK=1
powershell -NoProfile -Command "Add-Type -AssemblyName System.Drawing; Get-ChildItem '$out\*.bmp' | % { \$i=[System.Drawing.Bitmap]::new(\$_.FullName); \$i.Save(\$_.FullName.Replace('.bmp','.png'), [System.Drawing.Imaging.ImageFormat]::Png); \$i.Dispose(); Remove-Item \$_.FullName }"
ls $out
