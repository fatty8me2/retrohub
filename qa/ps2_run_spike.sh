#!/bin/bash
# usage: run.sh <rom> <extra args...>
ROM="$1"; shift
exec build/bin/Release/retrorun.exe --core scratch/cores/pcsx2_libretro.dll --rom "$ROM" --save-dir scratch/sv --state-dir scratch/st --system-dir scratch/sys --headless \
 --opt "pcsx2_renderer=Software (SW)" --opt pcsx2_mtvu=disabled --opt pcsx2_sw_renderer_threads=0 --opt pcsx2_bios=SCPH-70012_BIOS_V12_USA_200.BIN --opt pcsx2_fastboot=enabled "$@"
