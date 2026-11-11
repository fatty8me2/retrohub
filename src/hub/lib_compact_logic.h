#pragma once
/* Pure layout rules for the library's top area on small windows (no ImGui). Unit tested by hub_libcompact_test.
 *
 * The toolbar keeps ONE label per control at every width. When a row is short of room, controls give up their text and become an
 * icon (the tooltip carries the full label), in this order: Friends' games, then the sort chip, then (compact windows only) Add games. */

namespace hubui::lc {

/* Windows shorter than this (pixels) or with a content column narrower than kNarrowSp spacing units get the compact top area. */
constexpr float kShortWindowPx = 760.f;
constexpr float kNarrowSp = 100.f;

inline bool compact_top(float display_h, float content_w, float sp1) {
    return display_h < kShortWindowPx || content_w < kNarrowSp * sp1;
}

/* Which controls are icon-only. level 0 = every label shown, 1 = friends icon, 2 = + sort icon, 3 = + Add games icon. */
struct Plan {
    int level = 0;
    bool friends_icon() const { return level >= 1; }
    bool sort_icon() const { return level >= 2; }
    bool add_icon() const { return level >= 3; }
    bool fits = false;   // search keeps at least min_search at this level
};

/* fixed: everything on the row that never shrinks (view tabs, switcher, utility buttons, gaps) with the Add games button at its
 * full width. sort_w / friends_w: full chip widths; icon_w: width of an icon-only chip; add_full / add_icon_w: Add games button.
 * max_level: 2 for the normal bar, 3 for the compact one. Picks the lowest level that leaves min_search for the search box; when none
 * does, returns max_level with fits = false (the caller wraps or lives with a short search box). */
inline Plan plan_row(float avail, float fixed, float sort_w, float friends_w, float icon_w, float add_full, float add_icon_w, float min_search, int max_level) {
    Plan p;
    for (int lvl = 0; lvl <= max_level; ++lvl) {
        const float chips = (lvl >= 2 ? icon_w : sort_w) + (lvl >= 1 ? icon_w : friends_w);
        const float add = lvl >= 3 ? add_icon_w : add_full;
        p.level = lvl;
        if (avail - fixed - chips - add >= min_search) { p.fits = true; return p; }
    }
    p.level = max_level;
    p.fits = false;
    return p;
}

/* The filter chips row in compact mode: one row, this tall (chip height plus a thin scrollbar when the row overflows). */
inline float chips_row_h(float chip_h, float scrollbar_h, bool overflows) { return chip_h + (overflows ? scrollbar_h : 0.f); }

/* Game page: body height left for the scrolling tab area once the fixed parts (header, buttons, tabs, footer) are placed. */
inline float body_room(float window_max_h, float used_above, float footer_h, float pad_bottom, float min_h) {
    const float r = window_max_h - used_above - footer_h - pad_bottom;
    return r < min_h ? min_h : r;
}

}  // namespace hubui::lc
