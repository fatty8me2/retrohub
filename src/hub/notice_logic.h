#pragma once
/* Toasts and friend cards while a dialog is open: the pure decisions, so hub_notice_test can check them. No ImGui and no app state here;
 * frame.cpp (draw_toasts) and friends_ui.cpp (fr_draw_toasts) feed them.
 *
 * A notice is never delayed or dropped because a dialog is open:
 *   - a toast that would lie over a real dialog goes to a clear spot (below it, above it or beside it, whichever fits inside the window);
 *     with no clear spot it is drawn where it always was, over the dialog;
 *   - a friend card (invite, ask, friend request, game night) is drawn as usual, except while it would lie over an open dialog or menu or
 *     be cut by the window's top edge. While hidden its clock stands still, so it still gets its whole time on screen afterwards.
 * A full-window page (the Traditional game page) is not a dialog: toasts and cards show over it as they do over the library. */

#include <algorithm>
#include <functional>
#include <vector>

namespace hubui {
namespace notice {

struct Box {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};
inline bool overlaps(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
inline bool inside(const Box& a, const Box& in) { return a.x0 >= in.x0 - 0.01f && a.y0 >= in.y0 - 0.01f && a.x1 <= in.x1 + 0.01f && a.y1 <= in.y1 + 0.01f; }

/* A real dialog or menu: a popup that covers less than 90 % of the window. A full-window page (the game page) is not one. */
constexpr float kFullWindowShare = 0.9f;
inline bool real_dialog(const Box& popup, float disp_w, float disp_h) { return popup.w() * popup.h() < disp_w * disp_h * kFullWindowShare; }

/* ---------------- toasts ---------------- */

enum class Spot { Default, Below, Above, Right, Left, Over };
inline const char* spot_name(Spot s) {
    switch (s) {
        case Spot::Default: return "default";
        case Spot::Below: return "below";
        case Spot::Above: return "above";
        case Spot::Right: return "right";
        case Spot::Left: return "left";
        default: return "over";
    }
}
struct Size {
    float w = 0.f, h = 0.f;
    bool fits = false;   // laid out without cutting any text
};
/* The toast laid out for a total width of at most max_w (its text wrapped narrower). */
using Measure = std::function<Size(float max_w)>;
struct Placed {
    Box box;
    Spot spot = Spot::Default;
};

/* Where one toast goes. `def` = its usual place (right-aligned, in its stack). `bounds` = the window minus a margin. `dialog` = the open real
 * dialog grown by the gap to keep, or null. `taken` = toasts already placed this frame. `top` = a toast that stacks from the top edge.
 * `measure` lays it out narrower for a spot beside the dialog; `side_min_w` = the narrowest such spot worth using.
 * Order: its usual place when that is clear; then, nearest its stack first, below the dialog (above for a top toast), above it (below),
 * to its right, to its left; each spot is filled from the stack's side and steps past toasts already there. Nothing clear: its usual
 * place, over the dialog (as before dialogs were avoided). */
inline Placed place_toast(const Box& def, const Box& bounds, const Box* dialog, const std::vector<Box>& taken, float gap, bool top,
                          const Measure& measure, float side_min_w) {
    auto hits_taken = [&](const Box& b) -> const Box* {
        for (const Box& t : taken)
            if (overlaps(b, t)) return &t;
        return nullptr;
    };
    if (!dialog || (!overlaps(def, *dialog) && !hits_taken(def))) return {def, Spot::Default};
    const Box& d = *dialog;
    /* fill the column [x0, x1] between zy0 and zy1: a bottom toast from zy1 upward, a top toast from zy0 downward */
    auto fill = [&](float x0, float x1, float zy0, float zy1, float h, Box* out) -> bool {
        float y = top ? zy0 : zy1 - h;
        for (int guard = 0; guard < 64; ++guard) {
            if (y < zy0 - 0.01f || y + h > zy1 + 0.01f) return false;
            const Box b{x0, y, x1, y + h};
            if (!inside(b, bounds) || overlaps(b, d)) return false;
            const Box* t = hits_taken(b);
            if (!t) { *out = b; return true; }
            y = top ? t->y1 + gap : t->y0 - gap - h;
        }
        return false;
    };
    const float w = def.w(), h = def.h();
    auto below = [&](Box* o) { return fill(def.x0, def.x1, d.y1, bounds.y1, h, o); };
    auto above = [&](Box* o) { return fill(def.x0, def.x1, bounds.y0, d.y0, h, o); };
    auto side = [&](bool right, Box* o) {
        const float avail = right ? bounds.x1 - d.x1 : d.x0 - bounds.x0;
        if (avail < side_min_w || avail <= 0.f) return false;
        Size s{w, h, true};
        if (w > avail) {
            if (!measure) return false;
            s = measure(avail);
            if (!s.fits || s.w > avail + 0.01f) return false;
        }
        const float x0 = right ? bounds.x1 - s.w : bounds.x0, x1 = x0 + s.w;
        /* vertically: from the toast's usual line, moving away from it past toasts already there */
        const float zy0 = top ? std::max(bounds.y0, def.y0) : bounds.y0, zy1 = top ? bounds.y1 : std::min(bounds.y1, def.y1);
        return fill(x0, x1, zy0, zy1, s.h, o);
    };
    Box b;
    if (top) {
        if (above(&b)) return {b, Spot::Above};
        if (below(&b)) return {b, Spot::Below};
    } else {
        if (below(&b)) return {b, Spot::Below};
        if (above(&b)) return {b, Spot::Above};
    }
    if (side(true, &b)) return {b, Spot::Right};
    if (side(false, &b)) return {b, Spot::Left};
    return {def, Spot::Over};
}

/* ---------------- friend cards ---------------- */

/* Hidden this frame: it would lie over an open dialog or menu (`popups`: the real ones only, see real_dialog), or its top would be above
 * `top_limit` (no room left above the cards under it: a short window, a big UI size). */
inline bool card_hidden(const Box& card, const std::vector<Box>& popups, float top_limit) {
    if (card.y0 < top_limit) return true;
    for (const Box& p : popups)
        if (overlaps(card, p)) return true;
    return false;
}
/* One frame of a card's clock. Hidden: its arrival time moves on with the frame (`dt`), so its age stands still and it is neither drawn nor
 * expired. Shown: drawn until its age passes `life`. The age is also what the Friends list's game night row reads, so the two agree. */
struct CardTick {
    bool draw = false;
    bool expired = false;
    double t0 = 0.0;
};
inline CardTick card_tick(bool hidden, double t0, double now, double dt, double life) {
    CardTick r;
    r.t0 = t0;
    if (hidden) {
        r.t0 = t0 + (dt > 0.0 ? dt : 0.0);
        return r;
    }
    if (now - t0 > life) {
        r.expired = true;
        return r;
    }
    r.draw = true;
    return r;
}

}  // namespace notice
}  // namespace hubui
