#pragma once
/* Controller paths onto controls only the mouse reached (ui2-padspots): the pure decisions, so hub_padspots_test can check them.
 * No ImGui and no app state here; frame.cpp (toast actions) and page_library.cpp (Sort chip) feed them. */

#include "state_slots.h"   // hubui::SortSpec

#include <cmath>
#include <vector>

namespace hubui {
namespace padspots {

/* ---- Toast actions ("Leave and join", "Play", "Retry") ----
 * The toast shows a Y glyph beside its action, and Y presses that action, the same way Y presses Undo on the "removed from library"
 * card. A different button from the one that asked (Start / A / X joined the invite), so a double press can never confirm by itself. */
constexpr double kToastPadDelay = 0.5;   // seconds on screen before Y counts: a press meant for the screen underneath, made as the toast pops up, is not taken

struct ToastPadIn {
    bool has_action = false;     // the toast carries an action
    bool pill_shown = false;     // its action pill is drawn this frame (fully faded in, not fading out): the mouse can click it too
    double age = 0.0;            // seconds since it appeared
};
struct ToastPadEnv {
    bool pad_mode = false;       // the player is on a controller (glyphs are shown)
    bool input_blocked = false;  // the hub is unfocused / a game owns the pad
    bool game_running = false;
    bool popup_open = false;     // a menu or dialog is open: its own buttons own the pad
    bool text_input = false;
    bool wizard = false;         // the setup wizard owns the pad
};
inline bool toast_pad_env_ok(const ToastPadEnv& e) {
    return e.pad_mode && !e.input_blocked && !e.game_running && !e.popup_open && !e.text_input && !e.wizard;
}
/* Index of the toast that shows the Y glyph (and that Y presses), or -1. The newest action toast decides: while it is younger than the
 * delay, Y presses nothing (never an older toast underneath it); one that is already fading out is passed over. */
inline int toast_pad_pick(const std::vector<ToastPadIn>& toasts, const ToastPadEnv& env) {
    if (!toast_pad_env_ok(env)) return -1;
    for (int i = (int)toasts.size() - 1; i >= 0; --i) {
        const ToastPadIn& t = toasts[(size_t)i];
        if (!t.has_action) continue;
        if (t.age < kToastPadDelay) return -1;
        if (!t.pill_shown) continue;   // fading out
        return i;
    }
    return -1;
}

/* ---- Library Sort chip ---- */
/* Direction arrow after the chip's words: true = pointing down (descending: newest, most played or Z first). The natural order of each
 * mode decides, and Reverse flips it; the List view's column arrows follow the same rule (Last played starts descending, Title ascending). */
inline bool sort_arrow_down(const SortSpec& s) {
    const bool natural_desc = s.mode == SortMode::LastPlayed || s.mode == SortMode::MostPlayed || s.mode == SortMode::RecentlyAdded;
    return natural_desc != s.reverse;
}
/* Spaces appended to the chip's words so the arrow has room inside the chip (the chip sizes itself from its text). */
inline int arrow_pad_spaces(float space_w, float need_w) {
    if (!(space_w > 0.f)) return 3;
    const int n = (int)std::ceil(need_w / space_w);
    return n < 1 ? 1 : n;
}
/* Reverse the order without the mouse: X on a controller, or the Menu key / Shift+F10 (the keyboard's right-click), while the chip
 * shows the focus ring. A left click, A and Enter keep cycling the order as before. */
struct SortReverseIn {
    bool focused = false;        // the Sort chip holds the nav focus
    bool ring_visible = false;   // ...and the focus ring shows (keyboard / pad focus, not just a mouse click)
    bool text_input = false;
    bool pad_x = false;          // rising edges this frame
    bool menu_key = false;
    bool shift_f10 = false;
};
inline bool sort_reverse_press(const SortReverseIn& in) {
    return in.focused && in.ring_visible && !in.text_input && (in.pad_x || in.menu_key || in.shift_f10);
}

}  // namespace padspots
}  // namespace hubui
