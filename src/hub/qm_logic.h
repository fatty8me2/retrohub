#pragma once
/* Pure logic for the View+Y quick menu's Speed and Volume rows (quickmenu.cpp). Header-only; covered by hub_qm_test. */
#include <algorithm>
#include <string>

#include "runner_ctl.h"

namespace hubqm {

constexpr int kSpeedSteps = 9;      // 0.25x 0.5x 1x 1.5x 2x 3x 5x 8x Max
constexpr int kSpeedNormal = 2;     // index of 1x: every session starts here
constexpr int kVolumeStep = 5;

/* index -> runner_ctl speed value (0 = max) */
inline double speed_value(int idx) {
    idx = std::max(0, std::min(kSpeedSteps - 1, idx));
    return idx == kSpeedSteps - 1 ? 0.0 : runctl::kSpeeds[idx];
}
inline std::string speed_text(int idx) {   // "2x", "0.5x", "Max"
    std::string l = runctl::speed_label(speed_value(idx));   // "Speed 2x" / "Speed max"
    if (l.size() > 6) l = l.substr(6);
    if (l == "max") return "Max";
    return l;
}
inline int speed_step(int idx, int dir) { return std::max(0, std::min(kSpeedSteps - 1, idx + (dir < 0 ? -1 : dir > 0 ? 1 : 0))); }
inline int volume_step(int v, int dir) { return std::max(0, std::min(100, v + (dir < 0 ? -kVolumeStep : dir > 0 ? kVolumeStep : 0))); }

/* External emulators have no control channel: the hub scales the emulator's Windows audio session. `base` is the volume the
 * emulator already applies itself (Dolphin: -C Dolphin.DSP.Volume at launch; the others: 100), `cur` the slider.
 * Returns the session volume 0..1 (never boosts above what the emulator plays). */
inline float ext_session_scalar(int cur, int base) {
    cur = std::max(0, std::min(100, cur)); base = std::max(0, std::min(100, base));
    if (base <= 0) return cur > 0 ? 1.f : 0.f;
    return std::min(1.f, (float)cur / (float)base);
}

/* Speed: built-in runners only (control channel), and never online. */
inline bool speed_available(bool has_ctl, bool online) { return has_ctl && !online; }

}  // namespace hubqm
