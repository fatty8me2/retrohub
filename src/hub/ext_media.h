#pragma once
/* Quick-menu helpers for the external emulators (Dolphin, Cemu, Azahar, PCSX2), which have no runner_ctl channel:
 *  - capture_*: one picture of the emulator's window, saved as PNG (Windows Graphics Capture; PrintWindow as a fallback).
 *  - set_audio: scale / mute the emulator's Windows audio session(s) (per-app volume, like the Volume Mixer). */
#include <string>
#include <vector>

namespace extmedia {

/* Starts the capture on a worker thread. `hwnd` is the emulator's main window (HWND as void*). False if one is already running. */
bool capture_start(void* hwnd, const std::string& out_png_utf8);
/* 0 = nothing started, 1 = running, 2 = done (ok), 3 = failed; `msg` = error text when failed, the file path when done. Reading a
 * finished state resets it to 0. */
int capture_poll(std::string* msg);

/* Sets the master volume (0..1) and mute of every audio session that belongs to one of `pids` on the default output device.
 * Returns how many sessions were changed (0 = the emulator has not opened its audio yet). */
int set_audio(const std::vector<unsigned long>& pids, float scalar, bool mute);

}  // namespace extmedia
