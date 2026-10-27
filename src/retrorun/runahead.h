#pragma once
/* Run-ahead (retrorun, offline only), single instance: after the real frame the state is saved, 1-2 more frames run with the same
 * input (audio muted), the LAST of them is what gets displayed, then the saved state is restored. The player sees the game's
 * response 1-2 frames earlier than the core would show it. Pure logic over callbacks: unit tested (test_rewind.cpp). */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace runahead {

constexpr int kAuto = -1;   // setting value; 0 = off, 1, 2 = frames

/* Auto = 1 frame for nes / snes / gb / gbc / gba, 0 (off) for everything else. */
int auto_frames(const std::string& system);
/* Setting -> frames to run ahead for this system (0 = off). */
int effective(int setting, const std::string& system);
/* "off" / "0" / "1" / "2" / "auto" -> setting (false for anything else). */
bool parse(const std::string& s, int* setting);

/* Run n speculative frames after the real one. `saved` is the serialized state right after the real frame. */
void speculate(int n, const std::function<void()>& run, const std::function<void(bool)>& speculative,
               const std::function<bool(const void*, size_t)>& load, const std::vector<uint8_t>& saved);

}  // namespace runahead
