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

/* Instant replay: tap View/Back + LB ("save the last 30 s"). Start, Y, and either trigger (rewind = LT, boost = RT) must NOT be held.
 * RetroHub's own View+LB hold (1 s) only acts on Dolphin games, so the runners never conflict with it: a tap is long gone by 1 s. */
constexpr int kBtnLB = 9;             // SDL_CONTROLLER_BUTTON_LEFTSHOULDER
constexpr int kAxisLT = 4;
inline bool clip_combo_down(const PadState& s) {
    return s.b[kBtnBack] && s.b[kBtnLB] && !s.b[kBtnStart] && !s.b[kBtnYButton] && s.a[kAxisLT] < kTriggerOn && s.a[kAxisRT] < kTriggerOn;
}

/* Wii U "pad screen" toggle: hold View/Back + RB for kPadScreenHoldMs. Same exclusions as the replay combo (Start, Y, LT, RT must NOT be
 * held), so a game that uses Minus+R, and View+Start+RB on the way to the exit combo, never toggles the screen. RB is checked by SDL index. */
constexpr int kBtnRB = 10;            // SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
constexpr int kPadScreenHoldMs = 250;
inline bool pad_screen_combo_down(const PadState& s) {
    return s.b[kBtnBack] && s.b[kBtnRB] && !s.b[kBtnStart] && !s.b[kBtnYButton] && s.a[kAxisLT] < kTriggerOn && s.a[kAxisRT] < kTriggerOn;
}

/* XInput state -> the PadState the combos above take (pure; RetroHub reads Xbox pads directly while a game has the focus).
 * wButtons bits: DPAD 0x1-0x8, START 0x10, BACK 0x20, LTHUMB 0x40, RTHUMB 0x80, LB 0x100, RB 0x200, A 0x1000, B 0x2000, X 0x4000, Y 0x8000.
 * Triggers are 0..255 and scaled to the SDL range 0..32767 (so kTriggerOn 16000 is a byte value of about 125). */
inline PadState pad_from_xinput(unsigned short w, unsigned char lt, unsigned char rt) {
    PadState s;
    s.b[0] = (w & 0x1000) != 0; s.b[1] = (w & 0x2000) != 0; s.b[2] = (w & 0x4000) != 0; s.b[kBtnYButton] = (w & 0x8000) != 0;
    s.b[kBtnBack] = (w & 0x20) != 0; s.b[kBtnStart] = (w & 0x10) != 0;
    s.b[kBtnLB] = (w & 0x100) != 0; s.b[kBtnRB] = (w & 0x200) != 0;
    s.a[kAxisLT] = lt * 32767 / 255; s.a[kAxisRT] = rt * 32767 / 255;
    return s;
}

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

/* Pad-screen toggle with its hold requirement: feed it "some pad satisfies pad_screen_combo_down" each poll; true once per hold. */
class PadScreenCombo {
  public:
    bool update(bool down, long long now_ms) { return c_.update(down, now_ms); }
    void reset() { c_.reset(); }
  private:
    ExitCombo c_{kPadScreenHoldMs};
};

}  // namespace hubctl
