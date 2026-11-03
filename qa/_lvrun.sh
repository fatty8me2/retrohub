#!/bin/bash
# usage: _lvrun.sh <view> <size> <outname> [pad sequence]
export HUB_SHOT_VAULT="$TEMP/lv_vault"; mkdir -p "$HUB_SHOT_VAULT/_account"
export HUB_SHOT_FAKE_LIB=500 HUB_SHOT_FAKE_AZ=1 HUB_SHOT_FAKE_ART=1 SSB64_MUTE=1 RETRORUN_MUTE=1 N64RUN_MUTE=1
export HUB_SHOT_LIBVIEW=$1 HUB_SHOT_SIZE=$2 HUB_SCREENSHOT="$TEMP/lvs/$3.bmp" HUB_SHOT_TRACE="$TEMP/lvs/$3.trace"
rm -f "$HUB_SHOT_TRACE"
[ -n "$4" ] && export HUB_SHOT_PAD="$4" HUB_SHOT_SECS=${5:-12} || export HUB_SHOT_SECS=4
./build/bin/Release/RetroHub.exe >/dev/null 2>&1
