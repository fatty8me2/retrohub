#pragma once
/* One process-wide lock for bursts of synthetic keyboard input (the Wii U pad-screen chord and the PSP state keys in launch.cpp, the Dolphin
 * speed hotkey burst in launch.cpp, the Dolphin state chords in quickmenu.cpp). Two bursts running at once interleave their key-down /
 * key-up events and leave a modifier stuck or send the wrong chord, so each burst holds this for as long as it is pressing keys.
 * Bursts on a worker thread may lock() and wait; a burst on the UI frame thread must try_lock() and skip when busy, never block. */
#include <mutex>

namespace hubinput {

inline std::mutex& synth_input_mutex() {
    static std::mutex m;
    return m;
}

}  // namespace hubinput
