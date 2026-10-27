#pragma once
/* Universal "leave the game" pad combo: hold Back/View + Start/Menu (SDL BACK + START on the same pad) for 1 second.
 * Pure logic, no SDL: the runners feed it PadState each poll; RetroHub feeds it the pads it watches while an external
 * emulator runs. Fires exactly once per hold; releasing either button (or all pads vanishing) resets it. */

#include "controls.h"

namespace hubctl {

constexpr int kBtnBack = 4;     // SDL_CONTROLLER_BUTTON_BACK
constexpr int kBtnStart = 6;    // SDL_CONTROLLER_BUTTON_START
constexpr int kExitHoldMs = 1000;

/* XInput wButtons (XINPUT_GAMEPAD_BACK 0x20, XINPUT_GAMEPAD_START 0x10): RetroHub's watcher reads Xbox pads directly. */
inline bool xinput_combo_down(unsigned short w) { return (w & 0x30) == 0x30; }
inline bool exit_combo_down(const PadState& s) { return s.b[kBtnBack] && s.b[kBtnStart]; }

/* Quick boost: hold View/Back + right trigger. Start (exit) and Y (RetroHub's game menu) must NOT be held, so the combos never overlap. */
constexpr int kBtnYButton = 3;        // SDL_CONTROLLER_BUTTON_Y
constexpr int kAxisRT = 5;      // righttrigger
constexpr int kTriggerOn = 16000;
inline bool boost_combo_down(const PadState& s) { return s.b[kBtnBack] && s.a[kAxisRT] > kTriggerOn && !s.b[kBtnStart] && !s.b[kBtnYButton]; }

class ExitCombo {
  public:
    explicit ExitCombo(int hold_ms = kExitHoldMs) : hold_(hold_ms) {}
    /* `down`: some pad holds the combo right now. `now_ms`: any monotonic millisecond clock. Returns true once when the
     * hold reaches the threshold. */
    bool update(bool down, long long now_ms) {
        if (!down) { held_ = false; fired_ = false; return false; }
        if (!held_) { held_ = true; since_ = now_ms; }
        if (!fired_ && now_ms - since_ >= hold_) { fired_ = true; return true; }
        return false;
    }
    /* 0..1 while held (for an on-screen hint), 0 otherwise. */
    float progress(long long now_ms) const {
        if (!held_) return 0.f;
        float p = (float)(now_ms - since_) / (float)hold_;
        return p < 0.f ? 0.f : p > 1.f ? 1.f : p;
    }
    bool held() const { return held_; }
    void reset() { held_ = fired_ = false; }
  private:
    int hold_;
    bool held_ = false, fired_ = false;
    long long since_ = 0;
};

}  // namespace hubctl
