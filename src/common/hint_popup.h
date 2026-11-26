#pragma once
/* Click-through "controller tips" card for a RUNNER's own game window (n64run draws it over the plugin window it owns; retrorun
 * draws into its own frame instead, see start_hint.h). A layered, no-activate, transparent tool window in the bottom-left corner,
 * OWNED by the game window: NOT topmost, never raised, never focused, no input, hidden and destroyed with the game window. It sits
 * exactly as high as the game does, so it can never appear above another app. Exclusive-fullscreen games simply do not show it.
 *
 * Nothing is ever drawn over a game RetroHub does not own: with runner=false (the hub, for Dolphin / PCSX2 / Azahar / Cemu /
 * mGBA / BattleShip) tick() finishes at once and the hub shows the tips in its own window before the game's window exists.
 * Call tick() every ~100 ms from the thread that owns the popup; the object creates and destroys its window on that thread. */
#ifdef _WIN32
#include <windows.h>
#endif

#include "plat/plat.h"
#include "start_hint.h"

namespace starthint {

#if RH_WIN
using NativeWindow = HWND;
using TickMs = ULONGLONG;
#else
using NativeWindow = void*;           // macOS: there is no native popup yet (TODO(macOS lane 6): native popup); Popup finishes at once
using TickMs = unsigned long long;
#endif

class Popup {
  public:
    ~Popup() { close(); }
    /* target: the game's main window (null/hidden/minimised = not ready yet). Safe to keep calling after it finished. */
    void tick(NativeWindow target, bool last_input_keyboard, bool runner);
    bool done() const { return state_ == Done; }
    void close();
  private:
    enum State { Waiting, Showing, Done } state_ = Waiting;
    TickMs t_wait_ = 0, t_start_ = 0;
    NativeWindow hwnd_ = nullptr;
    Bitmap bmp_;
    float scale_ = 0.f;
    bool create(NativeWindow owner);
};

}  // namespace starthint
