#pragma once
/* Pure logic for the View+Y quick menu's Speed and Volume rows (quickmenu.cpp). Header-only; covered by hub_qm_test. */
#include <algorithm>
#include <string>
#include <vector>

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

/* ---- Dolphin (no control channel): speed through its own emulation-speed hotkeys, bound by RetroHub to Ctrl+Shift+F13 / F14 ----
 * Verified against dolphin-emu/dolphin tag 2509 (the bundled build): DolphinQt/HotkeyScheduler.cpp ~519-535: Decrease Emulation
 * Speed = EmulationSpeed - 0.1 (ignored when the result would be <= 0, so the floor is 0.1), Increase = + 0.1 (no cap); a result
 * within 0.95..1.05 snaps to exactly 1.0. Presses are edge-triggered (Core/HotkeyManager.cpp IsPressed), so each needs a release.
 * "Disable Emulation Speed Limit" (HK_TOGGLE_THROTTLE, default TAB) is HOLD-to-disable, not a toggle, so there is no Max step for
 * Dolphin: the row stops at 8x. The session starts at 1.0 (written to Dolphin.ini every launch). Our steps map to the nearest
 * reachable value, counted in 0.1 presses from 1x. */
constexpr int kDolphinSteps = kSpeedSteps - 1;                        // 0.25x .. 8x (no Max)
inline int dolphin_tenths(int idx) {                                  // target speed in tenths of 1x
    static const int t[kDolphinSteps] = {3, 5, 10, 15, 20, 30, 50, 80};   // 0.25x -> 0.3 (nearest reachable, ties up)
    idx = std::max(0, std::min(kDolphinSteps - 1, idx));
    return t[idx];
}
inline std::string dolphin_speed_text(int idx) {                      // what Dolphin really runs at ("0.3x" for our 0.25x)
    int t = dolphin_tenths(idx);
    return t % 10 == 0 ? std::to_string(t / 10) + "x" : std::to_string(t / 10) + "." + std::to_string(t % 10) + "x";
}
inline int dolphin_speed_step(int idx, int dir) { return std::max(0, std::min(kDolphinSteps - 1, idx + (dir < 0 ? -1 : dir > 0 ? 1 : 0))); }
enum class DolphinKey { Increase, Decrease };
inline int dolphin_vk(DolphinKey k) { return k == DolphinKey::Increase ? 0x7C : 0x7D; }              // VK_F13 / VK_F14
inline int dolphin_scan(int vk) { return vk == 0x7C ? 0x64 : vk == 0x7D ? 0x65 : 0; }               // DIK_F13 / DIK_F14 (DirectInput reads scancodes)
constexpr int kDolphinScanCtrl = 0x1D, kDolphinScanShift = 0x2A;
// Hotkeys.ini keys are "<group>/<control>" (ControlGroup::LoadConfig: base + name + "/" + c->name; group "Emulation Speed").
inline const char* dolphin_hotkey_name(DolphinKey k) { return k == DolphinKey::Increase ? "Emulation Speed/Increase Emulation Speed" : "Emulation Speed/Decrease Emulation Speed"; }
inline const char* dolphin_hotkey_expr(DolphinKey k) { return k == DolphinKey::Increase ? "@(Ctrl+Shift+F13)" : "@(Ctrl+Shift+F14)"; }
/* Dolphin's current speed step. `cur` is updated to `to`; returns the key presses (in order) that get there. */
struct DolphinSpeed { int cur = kSpeedNormal; };
inline int dolphin_idx(const DolphinSpeed& s) { return s.cur; }
inline std::vector<DolphinKey> dolphin_plan(DolphinSpeed& s, int to) {
    std::vector<DolphinKey> v;
    to = std::max(0, std::min(kDolphinSteps - 1, to));
    int d = dolphin_tenths(to) - dolphin_tenths(s.cur);
    for (int i = 0; i < (d < 0 ? -d : d); ++i) v.push_back(d < 0 ? DolphinKey::Decrease : DolphinKey::Increase);
    s.cur = to;
    return v;
}

/* The Speed row in the Game menu. Built-in runners and Dolphin have it (offline only); other emulators show it disabled. */
enum class SpeedRow { Hidden, Enabled, Online, Unsupported };
inline SpeedRow speed_row(bool has_ctl, bool is_dolphin, bool online, bool external) {
    if (has_ctl || is_dolphin) return online ? SpeedRow::Online : SpeedRow::Enabled;
    return external ? SpeedRow::Unsupported : SpeedRow::Hidden;
}

}  // namespace hubqm
