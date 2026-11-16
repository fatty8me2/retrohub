#pragma once
/* Game page layout rules (no ImGui), shared by both hosts of the page (game_page_view.cpp): the full-window page over the hub
 * (Traditional) and the Library desk's right-hand pane. Unit tested by hub_gamepage_test.
 *
 *  - The pinned header shrinks on a short sheet (a 1024x600 or 1280x720 window, a UI size of 150 % or more, the desk pane): a smaller
 *    cover, a one-line title, the buttons beside the title, and round icon buttons when the labels do not fit. The scrolling body keeps
 *    the rest of the height.
 *  - The body keeps text and option rows at a readable width on a wide sheet; the About tab gets a second column when there is room.
 * Every length is in pixels; `u` is one spacing unit (ui::sp(1)), so the rules scale with the UI size. */

#include <algorithm>

namespace hubui::gp {

enum class Tier { Full, Compact, Tight };

/* How roomy the header may be, from the sheet's height. Full: 1080p (and 900p) at 100 %. Compact: 720p and 600p at 100 %, 1080p at
 * 150 % and 200 %. Tight: 720p and 600p at 150 %, 900p at 200 %, and the desk pane on a 1024x600 window. */
inline Tier tier_for(float sheet_h, float u) {
    if (sheet_h >= 95.f * u) return Tier::Full;
    if (sheet_h >= 64.f * u) return Tier::Compact;
    return Tier::Tight;
}

struct HeaderIn {
    float avail_w = 0.f;   // the header's width
    float sheet_h = 0.f;   // the sheet's height (the whole window for the full-window page, the pane's height in the desk)
    float u = 8.f;         // one spacing unit
    float play_w = 0.f;    // Play / Resume with its label
    float online_w = 0.f;  // Play Online (or "Offline only" / "Online soon") with its label
    float more_w = 0.f;    // More with its label
    float close_w = 0.f;   // Close with its label; 0 = no Close button (the desk pane)
    float aspect = 1.3f;   // cover width / height
    float title_w = 0.f;   // the title's width on one line in the compact title font (0 = unknown): icons win when they let it show whole
};

struct HeaderPlan {
    Tier tier = Tier::Full;
    float cover_h = 0.f, cover_w = 0.f;   // 0 = no cover
    float text_x = 0.f;    // left edge of the title column, from the header's left
    float btn_h = 0.f;     // button height
    float gap = 0.f;       // between buttons
    bool icons = false;    // Play Online, More and Close are round icon buttons (their names in tooltips); Play keeps its label
    bool beside = false;   // the buttons sit at the right end of the title line (true) or on a row under the title text (false)
    float buttons_w = 0.f; // the button row's width as planned
    int title_lines = 1;   // the title wraps to at most this many lines (then "...")
    bool time_line = true; // the play-time line has a line of its own (false: it joins the facts line)
};

inline float title_min_w(float u) { return 28.f * u; }   // the title column never gets narrower than this beside the buttons

inline HeaderPlan plan_header(const HeaderIn& in) {
    const float u = in.u;
    HeaderPlan p;
    p.tier = tier_for(in.sheet_h, u);
    p.cover_h = p.tier == Tier::Full ? 16.f * u : p.tier == Tier::Compact ? 8.5f * u : 0.f;
    if (in.avail_w < 60.f * u) p.cover_h = 0.f;   // a very narrow sheet: the title gets the width
    p.cover_w = p.cover_h * in.aspect;
    p.text_x = p.cover_w > 0.f ? p.cover_w + 2.5f * u : 0.f;
    p.btn_h = p.tier == Tier::Full ? 5.5f * u : p.tier == Tier::Compact ? 5.f * u : 4.5f * u;
    p.gap = 1.25f * u;
    const int extra = in.close_w > 0.f ? 3 : 2;   // buttons after Play
    const float full = in.play_w + in.online_w + in.more_w + in.close_w + p.gap * (float)extra;
    const float icons = in.play_w + (p.btn_h + p.gap) * (float)extra;
    const float text_w = std::max(0.f, in.avail_w - p.text_x);
    const float title_gap = 2.f * u;
    if (p.tier == Tier::Full) {   // room to spare: title, facts and play time, then the buttons under them
        p.beside = false;
        p.icons = full > text_w;
        p.title_lines = 2;
    } else if (text_w - full - title_gap >= title_min_w(u) &&
               !(in.title_w > text_w - full - title_gap && in.title_w <= text_w - icons - title_gap)) {   // labels, unless only icons keep the title whole
        p.beside = true;
    } else if (text_w - icons - title_gap >= title_min_w(u)) {
        p.beside = true;
        p.icons = true;
    } else {
        p.beside = false;
        p.icons = full > text_w;
        p.title_lines = p.tier == Tier::Compact ? 2 : 1;
    }
    p.buttons_w = p.icons ? icons : full;
    p.time_line = p.tier != Tier::Tight;
    return p;
}

/* ---- body ---- */

/* Text and option rows stop at about 900 px (100 % UI size), like the kit's option rows. */
inline float readable_w(float avail, float u) { return std::min(avail, 112.f * u); }

/* The About tab on a wide body: the main column (description, the game's sections) and a side column (play-time stats, facts). */
struct AboutCols {
    bool side = false;
    float main_w = 0.f;
    float side_x = 0.f;    // from the body's left
    float side_w = 0.f;
};
inline AboutCols about_columns(float avail, float u) {
    AboutCols c;
    const float main_min = 80.f * u, side_min = 36.f * u, side_max = 56.f * u, gap = 4.f * u;
    if (avail >= main_min + gap + side_min) {
        c.side = true;
        c.side_w = std::max(side_min, std::min(side_max, avail * 0.32f));
        c.side_w = std::min(c.side_w, avail - gap - main_min);   // the main column keeps at least main_min
        c.main_w = std::min(readable_w(avail, u), avail - gap - c.side_w);
        c.side_x = c.main_w + gap;
    } else {
        c.main_w = readable_w(avail, u);
    }
    return c;
}

/* ---- hosts ---- */

/* What differs between the two hosts. The full-window page is modal: it has Close and a footer with the controller hints (it covers
 * the hub's footer). The desk pane sits beside the game list: no Close, and the hub's own footer carries its hints. */
struct HostMetrics {
    bool close_button = true;
    bool footer_hints = true;
    bool keep_tab = false;     // another game keeps the shown tab when it has it (the pane follows the list); the page opens on About
};
inline HostMetrics host_metrics(bool pane) {
    HostMetrics m;
    m.close_button = !pane;
    m.footer_hints = !pane;
    m.keep_tab = pane;
    return m;
}

}  // namespace hubui::gp
