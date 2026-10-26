#pragma once
/* Click-through "controller tips" popup for games RetroHub cannot draw into (see start_hint.h). A layered, topmost,
 * no-activate, transparent tool window placed in the bottom-left corner of the game window. It never takes focus and never
 * receives input. Exclusive-fullscreen games simply do not show it (nothing is forced). Call tick() every ~100 ms from the
 * thread that owns the popup; the object creates and destroys its window on that thread. */
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
    bool create(int w, int h);
};

}  // namespace starthint
