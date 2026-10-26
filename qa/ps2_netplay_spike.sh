#!/bin/bash
# usage: net.sh <tag> <rom> <frames>
T="$1"; ROM="$2"; F="$3"
export RETRORUN_MUTE=1 RETROHUB_QA=1 HUBNET_LOOPBACK=1 RETROHUB_PS2_NETPLAY=1
ROOM=$(head -c 400 /dev/urandom | tr -dc 'A-HJ-NP-Z2-9' | head -c 6)-1
for s in 0 1; do
  D=scratch/net_${T}_$s; rm -rf $D; mkdir -p $D/sv $D/st; cp -r scratch/sys $D/sys
  ( build/bin/Release/retrorun.exe --core scratch/cores/pcsx2_libretro.dll --rom "$ROM" --save-dir $D/sv --state-dir $D/st --system-dir $D/sys --headless \
   --opt "pcsx2_renderer=Software (SW)" --opt pcsx2_mtvu=disabled --opt pcsx2_sw_renderer_threads=0 --opt pcsx2_bios=SCPH-70012_BIOS_V12_USA_200.BIN --opt pcsx2_fastboot=enabled \
   --frames $F --fuzz-input --net ws://127.0.0.1:8793 --room $ROOM --slot $s --players 2 --ports 0,1,-1,-1 --delay auto --log scratch/net_${T}_$s.log >/dev/null 2>&1; echo "rc$s=$?" >> scratch/net_${T}.rc ) &
done
wait
