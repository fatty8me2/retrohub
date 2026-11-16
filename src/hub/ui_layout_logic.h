#pragma once
/* UI kit layout math (pure: no ImGui, no SDL). Unit-tested by hub_uikit_test.
 *
 * Consumer guide: you normally do not call these yourself. ui_scroll.h, ui_sheet.h and ui_controls.h use them so that every screen
 * scrolls, pages, splits and lines up option rows the same way. Call them directly only when you hand-draw something that must match
 * the kit (for example a custom list that pages like a ui::scroll_begin area). All sizes are pixels; `unit` is ui::sp(1).
 *
 *   float s = hubui::kit::page_target(scroll, scroll_max, view_h, +1);        // where RT takes a scroll area
 *   hubui::kit::RowOut r = hubui::kit::option_columns({avail, label_w, ctl_w, ctl_min, label_min, gap});
 */

#include <algorithm>
#include <cmath>

#include "homescroll_logic.h"   // hs::spring_step: the eased scroll's spring

namespace hubui {
namespace kit {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---------------- scroll area ---------------- */

/* One LT / RT page: most of the visible height, so the last rows of the old page stay in sight. */
inline float page_step(float view_h) { return std::max(1.f, view_h * 0.85f); }

/* The scroll offset one page up (dir < 0) or down (dir > 0), clamped to [0, scroll_max]. */
inline float page_target(float scroll, float scroll_max, float view_h, int dir) {
    const float mx = std::max(0.f, scroll_max);
    if (dir == 0) return clampf(scroll, 0.f, mx);
    return clampf(scroll + (dir > 0 ? 1.f : -1.f) * page_step(view_h), 0.f, mx);
}

/* Does a page move in `dir` have anywhere to go? */
inline bool can_page(float scroll, float scroll_max, int dir) {
    if (dir > 0) return scroll < scroll_max - 0.5f;
    if (dir < 0) return scroll > 0.5f;
    return false;
}

/* Room kept between a controller-focused item and the edge of the view: about one row, never more than a quarter of the view. */
inline float focus_margin(float view_h, float unit) { return std::max(0.f, std::min(unit * 8.f, view_h * 0.25f)); }

/* The scroll offset that brings an item into view with `margin` to spare. Item and view are screen y at the CURRENT scroll.
 * Unchanged when the item already sits inside [view_top + margin, view_bot - margin]. The margin shrinks when the item leaves no room
 * for it; an item taller than the view is aligned to its top (unless it already covers the whole view). */
inline float follow_scroll(float scroll, float scroll_max, float view_top, float view_bot, float item_top, float item_bot, float margin) {
    const float room = std::max(0.f, view_bot - view_top);
    const float h = std::max(0.f, item_bot - item_top);
    const float mx = std::max(0.f, scroll_max);
    float s = scroll;
    if (h >= room) {
        if (!(item_top <= view_top && item_bot >= view_bot)) s = scroll + (item_top - view_top);
    } else {
        const float m = std::max(0.f, std::min(margin, (room - h) * 0.5f));
        if (item_top < view_top + m) s = scroll - ((view_top + m) - item_top);
        else if (item_bot > view_bot - m) s = scroll + (item_bot - (view_bot - m));
    }
    return clampf(s, 0.f, mx);
}

/* Overflows = there is something to scroll (a fraction of a pixel is not). */
inline bool overflows(float scroll_max) { return scroll_max > 0.5f; }

/* The scrollbar track inside ImGui's scrollbar rect (ImGui::ScrollbarEx insets it by up to 3 px each way). */
struct Track { float x0, y0, x1, y1; };
inline Track scrollbar_track(float x0, float y0, float x1, float y1) {
    const float ix = clampf(std::trunc((x1 - x0 - 2.f) * 0.5f), 0.f, 3.f), iy = clampf(std::trunc((y1 - y0 - 2.f) * 0.5f), 0.f, 3.f);
    return Track{x0 + ix, y0 + iy, x1 - ix, y1 - iy};
}

/* The thumb, placed exactly where ImGui::ScrollbarEx puts its grab (so the drawn thumb is where the mouse grabs it).
 * content_h = ImGui's ContentSize.y + 2 * WindowPadding.y, view_h = the inner height, min_thumb = style.GrabMinSize. */
struct Thumb { bool show = false; float y0 = 0.f, y1 = 0.f; };
inline Thumb scrollbar_thumb(float track_top, float track_bot, float view_h, float content_h, float scroll, float min_thumb) {
    Thumb t;
    const float track_h = track_bot - track_top;
    if (track_h <= 0.f || view_h <= 0.f || !overflows(content_h - view_h)) return t;
    const float win = std::max(std::max(content_h, view_h), 1.f);
    const float grab = clampf(track_h * (view_h / win), std::min(min_thumb, track_h), track_h);
    const float smax = std::max(1.f, content_h - view_h);
    const float ratio = clampf(scroll / smax, 0.f, 1.f);
    t.show = true;
    t.y0 = track_top + ratio * (track_h - grab);
    t.y1 = t.y0 + grab;
    return t;
}

/* ---------------- eased scroll (ScrollFlags_Eased) ---------------- */

/* The offset ImGui itself would scroll to for a pending ScrollTarget (one axis of CalcNextScrollFromScrollTargetAndClamp, edge snap
 * included): target = the requested content position, center_ratio = the point of the view that lands on it (0 top, 1 bottom),
 * view_full = the window's full height minus its decorations, snap = ImGui's edge snap distance (0 = none). */
inline float resolve_scroll_target(float target, float center_ratio, float view_full, float snap, float scroll_max) {
    const float mx = std::max(0.f, scroll_max);
    if (snap > 0.f) {
        const float snap_max = mx + view_full;
        if (target <= snap) target = target * center_ratio;                                     // close to the top: onto it
        else if (target >= snap_max - snap) target = target + (snap_max - target) * center_ratio;   // close to the end: onto it
    }
    const float s = std::floor(std::max(0.f, target - center_ratio * view_full) + 0.5f);
    return std::min(s, mx);
}

/* The motion of an eased area: wheel notches, LT / RT pages and focus moves set `target`; every frame step() carries `pos` toward it on
 * a critically damped spring (hs::spring_step: frame-rate independent, eases out with a little momentum). It always comes to rest
 * exactly on the target, so nothing animates forever. `snap` (Reduce motion, a dragged scrollbar) jumps. */
struct Glide {
    float pos = 0.f, vel = 0.f, target = 0.f;
    void reset(float at) { pos = target = std::max(0.f, at); vel = 0.f; }
    void to(float t, float scroll_max) { target = clampf(t, 0.f, std::max(0.f, scroll_max)); }
    void by(float d, float scroll_max) { to(target + d, scroll_max); }
    bool moving() const { return pos != target || vel != 0.f; }
    /* Returns true while it is still moving. */
    bool step(float dt, float scroll_max, bool snap) {
        const float mx = std::max(0.f, scroll_max);
        target = clampf(target, 0.f, mx);
        pos = clampf(pos, 0.f, mx);
        if (snap) { pos = target; vel = 0.f; }
        else hs::spring_step(pos, vel, target, dt);
        return moving();
    }
};

/* ---------------- flow rows ---------------- */

/* A row of buttons that wraps: does the next one (width w) still fit after the previous one, which ends at prev_end, with `gap` between
 * them, inside a row that ends at `right`? False = start a new line. */
inline bool flow_fits(float prev_end, float gap, float w, float right) { return prev_end + gap + w <= right + 0.5f; }

/* A header that stays put above a scrolling body (e.g. filters above results) only when the body keeps at least min_body of height under
 * it; otherwise everything scrolls together. header_h < 0 = not measured yet (scroll together until it is). */
inline bool pin_header(float view_h, float header_h, float min_body) { return header_h >= 0.f && view_h - header_h >= min_body; }

/* ---------------- page sheet ---------------- */

/* Header on top, footer at the bottom, the body takes ALL the height between them. top / bottom are the sheet's inner edges
 * (padding already removed), header_h the measured header, footer_h the footer (measured last frame), gap the space either side of
 * the body. A sheet too short for header + footer gives the body 0 and lets the footer overlap the bottom edge (it is clipped). */
struct Split { float body_y = 0.f, body_h = 0.f, footer_y = 0.f; };
inline Split sheet_split(float top, float bottom, float header_h, float footer_h, float gap) {
    Split s;
    s.body_y = top + std::max(0.f, header_h) + (header_h > 0.f ? gap : 0.f);
    s.footer_y = bottom - std::max(0.f, footer_h);
    const float body_bottom = s.footer_y - (footer_h > 0.f ? gap : 0.f);
    s.body_h = std::max(0.f, body_bottom - s.body_y);
    if (s.footer_y < s.body_y) s.footer_y = s.body_y;
    return s;
}

/* ---------------- dialog ---------------- */

enum DialogClass { DlgSmall = 0, DlgMedium = 1, DlgLarge = 2 };
/* Width of a dialog: 52 / 66 / 88 units (416 / 528 / 704 px at 100 %), never more than 92 % of the window. */
inline float dialog_width(int size_class, float unit, float display_w) {
    static const float k[3] = {52.f, 66.f, 88.f};
    const int c = size_class < 0 ? 0 : (size_class > 2 ? 2 : size_class);
    return std::max(1.f, std::min(k[c] * unit, display_w * 0.92f));
}
/* A dialog is never taller than 90 % of the window. */
inline float dialog_max_h(float display_h) { return std::max(1.f, display_h * 0.9f); }
/* Height left for the scrolling middle once the pinned title and button rows are placed (at least min_body). */
inline float dialog_body_max(float max_h, float pad_y, float title_h, float buttons_h, float gaps, float min_body) {
    return std::max(min_body, max_h - 2.f * pad_y - title_h - buttons_h - gaps);
}
/* A wrapping button row: does an item `next_w` wide still fit after the last one (right edge `item_right`, then `gap`) before `right_edge`? */
inline bool fits_on_line(float item_right, float gap, float next_w, float right_edge) { return item_right + gap + next_w <= right_edge + 0.5f; }

/* ---------------- option rows ---------------- */

/* Columns of an option group. All rows of a group share them: the label column starts at the row's left edge, the control column
 * ends at its right edge, both sized from the group's longest label and longest value so nothing is clipped.
 *   1. everything fits on one line: control at its natural width, labels get the rest;
 *   2. otherwise the control shrinks (its value wraps inside it) while it stays >= ctl_min and the labels stay on one line;
 *   3. otherwise both give way: the control takes about 45 % (>= ctl_min) and the labels wrap, if they keep >= label_min;
 *   4. otherwise the row wraps: the control goes under the label at full width (capped at its natural width). */
struct RowIn {
    float avail = 0.f;      // row width
    float label_w = 0.f;    // longest label line (+ its status pill) in the group
    float ctl_w = 0.f;      // longest control (value text + chrome) in the group
    float ctl_min = 0.f;    // narrowest acceptable control
    float label_min = 0.f;  // narrowest acceptable label column
    float gap = 0.f;        // space between the label column and the control
};
struct RowOut {
    bool wrap = false;      // control under the label
    float label_w = 0.f;    // width the label and help text may use
    float ctl_x = 0.f;      // control left edge, from the row's left edge
    float ctl_w = 0.f;      // control width
};
inline RowOut option_columns(const RowIn& in) {
    RowOut o;
    const float avail = std::max(0.f, in.avail);
    const float natural = std::max(0.f, in.ctl_w);
    if (in.label_w + in.gap + natural <= avail) {
        o.ctl_w = natural;
    } else if (avail - in.gap - in.label_w >= in.ctl_min && avail - in.gap - in.label_w > 0.f) {
        o.ctl_w = std::min(natural, avail - in.gap - in.label_w);
    } else {
        const float ctl = std::min(natural, std::max(in.ctl_min, avail * 0.45f));
        if (avail - in.gap - ctl >= in.label_min) o.ctl_w = ctl;
        else {
            o.wrap = true;
            o.ctl_w = std::min(natural, avail);
            o.ctl_x = 0.f;
            o.label_w = avail;
            return o;
        }
    }
    o.ctl_x = avail - o.ctl_w;
    o.label_w = std::max(0.f, o.ctl_x - in.gap);
    return o;
}

/* ---------------- steppers, lists, tabs ---------------- */

/* Next value of a stepper: dir -1 / +1 by `step`, clamped to [lo, hi]; with wrap a step past one end lands on the other end. */
inline int step_value(int v, int lo, int hi, int step, int dir, bool wrap) {
    if (hi < lo) std::swap(lo, hi);
    if (step <= 0) step = 1;
    const long long n = (long long)v + (long long)(dir > 0 ? step : (dir < 0 ? -step : 0));
    if (n > hi) return wrap && v >= hi ? lo : hi;
    if (n < lo) return wrap && v <= lo ? hi : lo;
    return (int)n;
}

/* LB / RB on a tab bar: the neighbouring tab, or -1 when there is none that way (without wrap). */
inline int tab_step(int cur, int n, int dir, bool wrap) {
    if (n <= 0 || dir == 0) return -1;
    const int c = cur < 0 ? 0 : (cur >= n ? n - 1 : cur);
    int t = c + (dir > 0 ? 1 : -1);
    if (t < 0) t = wrap ? n - 1 : -1;
    else if (t >= n) t = wrap ? 0 : -1;
    return t == c ? -1 : t;
}

/* An open dropdown keeps the D-pad inside its list: a move past the first or last option is swallowed. */
inline bool list_move_blocked(int idx, int n, int dir) {
    if (n <= 0) return true;
    return (dir < 0 && idx <= 0) || (dir > 0 && idx >= n - 1);
}

}  // namespace kit
}  // namespace hubui
