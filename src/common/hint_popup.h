#pragma once
/* Click-through "controller tips" card for a RUNNER's own game window (n64run draws it over the plugin window it owns; retrorun
 * draws into its own frame instead, see start_hint.h). A layered, no-activate, transparent tool window in the bottom-left corner,
 * OWNED by the game window: NOT topmost, never raised, never focused, no input, hidden and destroyed with the game window. It sits
 * exactly as high as the game does, so it can never appear above another app. Exclusive-fullscreen games simply do not show it.
 *
 * Nothing is ever drawn over a game RetroHub does not own: with runner=false (the hub, for Dolphin / PCSX2 / Azahar / Cemu /
 * mGBA / BattleShip) tick() finishes at once and the hub shows the tips in its own window before the game's window exists.
 * Call tick() every ~100 ms from the thread that owns the popup; the object creates and destroys its window on that thread. */
#include <windows.h>

#include "start_hint.h"

namespace starthint {

class Popup {
  public:
    ~Popup() { close(); }
    /* target: the game's main window (null/hidden/minimised = not ready yet). Safe to keep calling after it finished. */
    void tick(HWND target, bool last_input_keyboard, bool runner);
    bool done() const { return state_ == Done; }
    void close();
  private:
    enum State { Waiting, Showing, Done } state_ = Waiting;
    ULONGLONG t_wait_ = 0, t_start_ = 0;
    HWND hwnd_ = nullptr;
    Bitmap bmp_;
    float scale_ = 0.f;
    bool create(HWND owner);
};

}  // namespace starthint
