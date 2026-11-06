#pragma once
/* Cases library view (a console-style 3D carousel of game cases): the pure parts. No ImGui here, unit-tested in test_cases.cpp.
 * Slot pose maths (where each case stands for its distance from the centre), the case shapes per console, stepping with wrap / clamp,
 * hold-to-accelerate, the console tab list and its per-console selection memory, the flip / inspect mode machine, and the shuffle pick.
 * The drawing is shelf_draw.cpp (cases section), the integration with the All Games state is libview_ui.cpp. */

#include "libview_logic.h"
#include "shelf_logic.h"
#include "surprise_logic.h"

#include "state_slots.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hubui::cases {

constexpr int kWindow = 12;          // cases drawn on each side of the centre
constexpr float kFadeFrom = 8.6f;    // distance where the edge fade starts
constexpr float kFadeTo = 12.2f;     // distance where a case is fully faded out (never popping in or out)

/* ---- slot pose: where a case stands for its (signed, fractional) distance d from the centre ---- */
struct SlotPose {
    float x = 0;        // world offset from the centre, in world units (case height of a tall keep case = 1)
    float yaw = 0;      // turn about the vertical axis, radians: 0 = face-on, +ve = right of centre (front turned toward the centre)
    float z = 0;        // depth behind the centre case, world units (0 = the centre plane)
    float scale = 1;
    float alpha = 1;
};
inline float smoothstep01(float t) { t = std::max(0.f, std::min(1.f, t)); return t * t * (3.f - 2.f * t); }
inline SlotPose slot_pose(float d) {
    SlotPose p;
    const float a = std::fabs(d);
    const float sgn = d < 0.f ? -1.f : 1.f;
    // x: a Hermite curve to the first neighbour (0 -> 0.66 for a keep case; wider boxes add neighbour_x() on top, so it stays clear of the centre case's face, ends with the shelf pitch), then a tight pitch
    const float x1 = 0.66f, m0 = 1.0f, m1 = 0.16f, pitch = 0.16f;
    float x;
    if (a < 1.f) {
        const float t = a, t2 = t * t, t3 = t2 * t;
        x = (2 * t3 - 3 * t2 + 1) * 0.f + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * x1 + (t3 - t2) * m1;
    } else x = x1 + (a - 1.f) * pitch;
    p.x = sgn * x;
    // yaw: nearly flat at the centre, ~41 degrees for the first neighbour, close to side-on (75-78 degrees) further out
    const float ymax = 1.37f, k = 0.75f;
    p.yaw = sgn * ymax * (1.f - std::exp(-k * a));
    p.z = 0.55f * std::min(a, 1.f) * (2.f - std::min(a, 1.f)) + 0.11f * std::max(0.f, a - 1.f);
    p.scale = 1.f - 0.18f * std::min(a, 1.f) - 0.015f * std::min(std::max(a - 1.f, 0.f), 6.f);   // the neighbour is about 75% of the centre
    p.alpha = a <= kFadeFrom ? 1.f : 1.f - smoothstep01((a - kFadeFrom) / (kFadeTo - kFadeFrom));
    return p;
}

/* ---- case shapes ---- */
enum class CaseKind { Keep, Small, Jewel, Box };
inline CaseKind kind_for(shelf::Media m) {
    using shelf::Media;
    switch (m) {
        case Media::NesCart: case Media::SnesCart: case Media::N64Cart: case Media::GbCart: case Media::GbcCart: case Media::GbaCart: return CaseKind::Box;
        case Media::DsCase: case Media::N3dsCase: case Media::PspCase: return CaseKind::Small;
        case Media::Ps1Jewel: return CaseKind::Jewel;
        default: return CaseKind::Keep;   // GC / Wii U / PS2 / Switch / generic: the tall DVD-style keep case
    }
}
inline bool is_box(shelf::Media m) { return kind_for(m) == CaseKind::Box; }
inline bool has_sheen(shelf::Media m) { return !is_box(m); }   // clear plastic is glossy, cardboard is matte
struct CaseDims { float w = 0.71f, h = 1.f, d = 0.075f; };   // world units; a 190 mm tall keep case is 1.0 high
inline CaseDims case_dims(shelf::Media m) {
    using shelf::Media;
    switch (m) {
        case Media::GcCase: return {0.71f, 1.f, 0.068f};        // GameCube keep case (about 135 x 191 x 13 mm)
        case Media::Ps2Case: return {0.71f, 1.f, 0.075f};       // DVD keep case (135 x 190 x 14)
        case Media::WiiUCase: return {0.67f, 0.90f, 0.058f};    // Blu-ray keep case (the cover scans measure 0.71 w/h)
        case Media::SwitchCase: return {0.553f, 0.895f, 0.058f};// Switch keep case, slim (105 x 170 x 11 mm)
        case Media::Ps1Jewel: return {0.86f, 0.757f, 0.058f};   // jewel case (142 x 125 mm, wider than tall; drawn a little large)
        case Media::DsCase: return {0.76f, 0.65f, 0.07f};       // DS case: the cover scans are landscape (1.12 w/h)
        case Media::N3dsCase: return {0.76f, 0.65f, 0.072f};
        case Media::PspCase: return {0.54f, 0.90f, 0.068f};     // UMD case: tall and narrow (104 x 171 x 14)
        case Media::NesCart: return {0.67f, 0.93f, 0.16f};      // cardboard boxes: thicker than the plastic cases
        case Media::SnesCart: return {0.97f, 0.70f, 0.18f};     // landscape (184 x 132 x 35)
        case Media::N64Cart: return {0.94f, 0.68f, 0.19f};      // landscape (178 x 128 x 38)
        case Media::GbCart: case Media::GbcCart: return {0.66f, 0.66f, 0.15f};   // small square boxes (the cover scans are 1:1)
        case Media::GbaCart: return {0.66f, 0.66f, 0.15f};
        default: return {0.71f, 1.f, 0.075f};
    }
}

/* ---- case faces: where the cover art goes (pure; shared by cases_draw.cpp and the tests) ---- */
struct UvRect { float u0 = 0, v0 = 0, u1 = 1, v1 = 1; };
/* Share of the face width taken by the hinge strip of a plastic case (the spine side of the front, and of the back). */
inline float hinge_frac(CaseKind k) { return k == CaseKind::Jewel ? 0.066f : k == CaseKind::Small ? 0.05f : 0.045f; }
/* The cover window of the front face in face uv: the area the cover scan fills (cardboard boxes: nearly the whole face; plastic cases: inside the
 * hinge strip and the rim). Pixel-free so the nominal cover aspect below can be computed without a face. */
inline UvRect front_window(CaseKind k) {
    UvRect r;
    if (k == CaseKind::Box) { r.u0 = 0.028f; r.v0 = 0.028f; r.u1 = 1.f - 0.028f; r.v1 = 1.f - 0.03f; }
    else { r.u0 = hinge_frac(k) + 0.008f; r.v0 = 0.02f; r.u1 = 1.f - 0.024f; r.v1 = 1.f - 0.02f; }
    return r;
}
/* Width / height of the cover window of a console's case (what a scan of that console's cover is expected to measure). */
inline float nominal_cover_aspect(shelf::Media m) {
    const CaseDims d = case_dims(m);
    const UvRect r = front_window(kind_for(m));
    return (d.w * (r.u1 - r.u0)) / (d.h * (r.v1 - r.v0));
}
/* Where a whole cover scan of aspect `art_aspect` (w/h) goes inside a window of aspect `win_aspect` (both pixel aspects), as a rect in window
 * fractions (0..1). The scan is never cropped. A scan within ~15% of the console's nominal cover aspect is the case's own insert: it takes the
 * window as is when it is within 7% of its shape (a hair of stretch), else the window shrinks to the scan's shape (the plastic shows around it);
 * anything else is letterboxed (`letterbox` = true: the margin is painted with the insert colour, not the case colour). */
struct CaseArtFit { UvRect dst; bool letterbox = false; bool exact = false; };
inline CaseArtFit fit_art(float win_aspect, float art_aspect, float nominal) {
    CaseArtFit f;
    if (!(win_aspect > 0.f) || !(art_aspect > 0.f)) return f;
    const float rel = art_aspect / (nominal > 0.f ? nominal : win_aspect);
    f.letterbox = !(rel >= 0.85f && rel <= 1.15f);
    const float k = art_aspect / win_aspect;   // > 1: the scan is wider than the window
    if (k > 0.93f && k < 1.07f) { f.exact = true; return f; }
    if (k > 1.f) { const float h = 1.f / k; f.dst.v0 = (1.f - h) * 0.5f; f.dst.v1 = f.dst.v0 + h; }
    else { f.dst.u0 = (1.f - k) * 0.5f; f.dst.u1 = f.dst.u0 + k; }
    return f;
}
/* A square-ish scan (a Switch game's own icon) on a portrait case: the front is composed (banner, large icon, title) instead of a scan. */
inline bool is_square_art(int w, int h) { return w > 0 && h > 0 && (float)w / (float)h >= 0.9f && (float)w / (float)h <= 1.1f; }
inline bool is_portrait_case(shelf::Media m) { const CaseDims d = case_dims(m); return d.h >= d.w * 1.15f; }
/* uv sub-rects of a full cover scan (BACK | SPINE | FRONT left to right) from the pixel split fullcover_split() returns. */
struct FullParts { UvRect back, spine, front; };
inline FullParts full_cover_parts(int w, int back_x1, int front_x0) {
    FullParts p;
    const float iw = w > 0 ? 1.f / (float)w : 0.f;
    p.back = {0.f, 0.f, (float)back_x1 * iw, 1.f};
    p.spine = {(float)back_x1 * iw, 0.f, (float)front_x0 * iw, 1.f};
    p.front = {(float)front_x0 * iw, 0.f, 1.f, 1.f};
    return p;
}

/* Real heights differ a lot (a DS case is 0.76 of a keep case, a box 0.72): cases are drawn this much bigger so the centre case is always about
 * as tall as the stage allows, but only part-way (the smaller systems still look smaller). */
inline float display_scale(shelf::Media m) { const CaseDims d = case_dims(m); return std::min(std::pow(1.f / d.h, 0.6f), 1.0f / d.w); }   // wide boxes stay narrow enough for neighbours to fit
/* World x of the nearest neighbour (turned ~41 degrees, 0.82 size, 0.92 perspective at its depth) so that its inner edge clears the centre
 * case's face by a small gap. cs / ns = the display scales of the two cases. */
inline float neighbour_x(const CaseDims& c, float cs, const CaseDims& n, float ns) {
    const float persp = 0.922f, yaw1 = 0.72f, scale1 = 0.82f, gap = 0.04f;
    const float half_n = (0.5f * n.w * std::cos(yaw1) + 0.5f * n.d * std::sin(yaw1)) * scale1 * ns;
    return (0.5f * c.w * cs + gap) / persp + half_n;
}
constexpr float kNeighbourBase = 0.66f;   // slot_pose(1).x: enough for two keep cases

/* ---- the spring that moves the carousel ---- */
/* One frame of a soft, slightly under-damped spring toward `target` (the stiffer the further away it is, so a long run keeps up).
 * reduced_motion snaps. Returns true while still moving. */
inline bool pos_step(shelf::Spring& s, float target, float dt, bool reduced_motion = false) {
    if (reduced_motion || !(dt > 0.f)) {
        if (reduced_motion) { s.x = target; s.v = 0; }
        return s.x != target || s.v != 0.f;
    }
    dt = std::min(dt, 0.1f);
    const float err0 = std::fabs(target - s.x);
    const float k = 150.f * (1.f + 0.25f * std::min(err0, 12.f)), c = 2.f * 0.82f * std::sqrt(k);   // zeta ~ 0.82: a hint of overshoot
    const int steps = std::max(1, (int)std::ceil(dt / (1.f / 180.f)));
    const float h = dt / (float)steps;
    for (int i = 0; i < steps; ++i) {
        s.v += (k * (target - s.x) - c * s.v) * h;
        s.x += s.v * h;
    }
    if (std::fabs(target - s.x) < 0.0008f && std::fabs(s.v) < 0.012f) { s.x = target; s.v = 0; return false; }
    return true;
}

/* ---- stepping ---- */
/* One step of `dir` (+1 / -1) from `cur` among n cases. A fresh press at an end wraps around when `wrap`; otherwise it clamps. */
inline int step_index(int cur, int dir, int n, bool wrap) {
    if (n <= 0) return 0;
    cur = std::max(0, std::min(n - 1, cur));
    const int t = cur + (dir < 0 ? -1 : dir > 0 ? 1 : 0);
    if (t < 0) return wrap ? n - 1 : 0;
    if (t >= n) return wrap ? 0 : n - 1;
    return t;
}
/* Cases moved per repeat while a direction is held for `held` seconds: 1, then 2, 3, 5 as the hold goes on. */
inline int hold_boost(float held) { return held < 0.9f ? 1 : held < 1.8f ? 2 : held < 3.0f ? 3 : 5; }
/* Target index after moving `steps` cases (clamped, never wraps while held). */
inline int clamp_index(int i, int n) { return n <= 0 ? 0 : std::max(0, std::min(n - 1, i)); }
/* The cases to draw for a carousel position: [first, last] clamped to [0, n-1], first > last when n == 0. */
inline void window(float pos, int n, int* first, int* last) {
    *first = std::max(0, (int)std::floor(pos) - kWindow);
    *last = std::min(n - 1, (int)std::ceil(pos) + kWindow);
}
/* How far a jump may animate: a longer way snaps the carousel to within this distance first, then glides in. */
inline float jump_start(float pos, int target, float max_glide) {
    const float d = (float)target - pos;
    if (std::fabs(d) <= max_glide) return pos;
    return (float)target - (d < 0.f ? -max_glide : max_glide);
}

/* ---- console tabs ---- */
struct Tab { std::string sys; std::string label; int count = 0; };   // sys "" = All
inline const char* tab_label(const std::string& sys) {
    static const char* m[][2] = {{"n64", "N64"}, {"gc", "GameCube"}, {"ps1", "PS1"}, {"wiiu", "Wii U"}, {"3ds", "3DS"}, {"nds", "DS"}, {"ps2", "PS2"},
                                 {"psp", "PSP"}, {"switch", "Switch"}, {"nes", "NES"}, {"snes", "SNES"}, {"gba", "GBA"}, {"gbc", "GBC"}, {"gb", "Game Boy"}};
    for (auto& e : m) if (sys == e[0]) return e[1];
    return nullptr;
}
inline int console_rank(const std::string& sys) {
    static const char* o[] = {"n64", "gc", "ps1", "wiiu", "3ds", "nds", "ps2", "psp", "switch", "nes", "snes", "gba", "gbc", "gb"};
    for (int i = 0; i < (int)(sizeof(o) / sizeof(o[0])); ++i) if (sys == o[i]) return i;
    return 100;
}
/* "All" first, then every console that has games (count > 0, or `keep` = the active one) in the library's console order. */
inline std::vector<Tab> build_tabs(const std::map<std::string, int>& counts, int total, const std::string& keep) {
    std::vector<Tab> t;
    t.push_back({"", "All", total});
    std::vector<Tab> rest;
    for (const auto& kv : counts) {
        if (kv.first.empty() || (kv.second <= 0 && kv.first != keep)) continue;
        std::string lab = tab_label(kv.first) ? tab_label(kv.first) : kv.first;
        if (!tab_label(kv.first)) for (char& c : lab) c = (char)std::toupper((unsigned char)c);
        rest.push_back({kv.first, lab, kv.second});
    }
    std::stable_sort(rest.begin(), rest.end(), [](const Tab& a, const Tab& b) {
        const int ra = console_rank(a.sys), rb = console_rank(b.sys);
        return ra != rb ? ra < rb : a.sys < b.sys;
    });
    for (auto& r : rest) t.push_back(r);
    return t;
}
inline int tab_index(const std::vector<Tab>& tabs, const std::string& sys) {
    for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i].sys == sys) return (int)i;
    return 0;
}
/* LB / RB: the neighbouring tab, wrapping. */
inline int tab_step(int cur, int dir, int n) { return n <= 0 ? 0 : ((cur + (dir < 0 ? -1 : 1)) % n + n) % n; }

/* The last selected game of each console tab ("" = the All tab), so switching tabs lands where the player left off. */
struct TabMemory {
    std::map<std::string, std::string> last;
    void remember(const std::string& tab_sys, const std::string& game_sys, const std::string& key) {
        if (key.empty()) return;
        last[tab_sys] = key;
        last[game_sys] = key;   // the same game is also the last one seen on its own console's tab
        last[""] = key;
    }
    std::string recall(const std::string& tab_sys) const {
        auto it = last.find(tab_sys);
        return it == last.end() ? std::string() : it->second;
    }
};
/* Where to land after a tab change: the remembered game when it is in `keys` (the new tab's games, display order), else 0. */
inline int land_index(const std::vector<std::string>& keys, const std::string& remembered) {
    for (size_t i = 0; i < keys.size(); ++i) if (!remembered.empty() && keys[i] == remembered) return (int)i;
    return 0;
}

/* ---- browse / flip / inspect ---- */
enum class Mode { Browse, Flip, Inspect };
struct ModeState {
    Mode mode = Mode::Browse;
    float flip_target() const { return mode == Mode::Flip ? 3.14159265f : 0.f; }
};
/* X: tap flips the centre case to its back and back again (from Inspect, a tap goes back to browsing). */
inline Mode on_flip_tap(Mode m) { return m == Mode::Browse ? Mode::Flip : Mode::Browse; }
/* R3 / R / dragging the centre case: inspect; again (or B) returns. A flipped case cannot be inspected until it is flipped back. */
inline Mode on_inspect(Mode m) { return m == Mode::Inspect ? Mode::Browse : m == Mode::Browse ? Mode::Inspect : m; }
/* B: leaves flip / inspect and reports whether it consumed the press (otherwise B goes on to "back"). */
inline bool on_back(Mode* m) { if (*m == Mode::Browse) return false; *m = Mode::Browse; return true; }
/* X held for this long toggles the favourite instead of flipping. */
constexpr float kFavHold = 0.55f;
/* Moving to another case or console leaves flip / inspect. */
inline Mode on_move(Mode) { return Mode::Browse; }

/* Free rotation while inspecting: the stick / drag deltas turn the case (yaw unlimited, pitch limited). */
struct Spin { float yaw = 0, pitch = 0; };
inline Spin spin_apply(Spin s, float dx, float dy) {
    s.yaw += dx;
    s.pitch = std::max(-0.55f, std::min(0.55f, s.pitch + dy));
    return s;
}
/* The rest yaw to settle on when leaving inspect (nearest whole turn): the case swings back to face the viewer. */
inline float spin_home(float yaw) { const float tau = 6.28318531f; return std::round(yaw / tau) * tau; }

/* ---- shuffle ---- */
/* Index of a random game for L3 / Z: the Surprise-me weighting (games not played for a while, favourites a little likelier) over the
 * shown games, never the current one when there is a choice. u in [0,1). -1 for an empty list. */
struct ShuffleCand { std::string key; int64_t last_played = 0; bool favorite = false; };
inline int pick_shuffle(const std::vector<ShuffleCand>& c, int cur, int64_t now, double u) {
    if (c.empty()) return -1;
    if (c.size() == 1) return 0;
    std::vector<surprise::Cand> sc;
    sc.reserve(c.size());
    for (const ShuffleCand& x : c) sc.push_back({x.key, x.last_played, x.favorite});
    const std::string avoid = cur >= 0 && cur < (int)c.size() ? c[(size_t)cur].key : std::string();
    const surprise::Pick p = surprise::pick(sc, now, u, avoid);
    return p.index < 0 ? 0 : p.index;
}

/* ---- the strings under the centre case ---- */
/* "Nintendo 64  -  1999  -  1-4 players": console, then year and players when known. */
inline std::string subtitle_text(const std::string& console, int year, int players) {
    std::string s = console;
    if (year > 0) s += "  -  " + std::to_string(year);
    if (players == 1) s += "  -  1 player";
    else if (players > 1) s += "  -  1-" + std::to_string(players) + " players";
    return s;
}

/* ---- nav: sort sections, scrubber, quick jump ---- */
/* What the scrubber's marks name for each sort mode, and the pure pieces behind them: labels per game, runs of equal labels ("sections"),
 * previous / next section, index <-> scrubber fraction, typed-prefix lookup, the sort picker rows and the faster hold / wheel travel. */
enum class NavKind { Letters, Recency, Plays, Added, Years, Consoles };
inline NavKind nav_kind_for(SortMode m) {
    switch (m) {
        case SortMode::LastPlayed: return NavKind::Recency;
        case SortMode::MostPlayed: return NavKind::Plays;
        case SortMode::RecentlyAdded: return NavKind::Added;
        case SortMode::ReleaseYear: return NavKind::Years;
        case SortMode::Console: return NavKind::Consoles;
        default: return NavKind::Letters;
    }
}
/* Title: "A".."Z", everything else "#" (digits, symbols, non-ASCII). */
inline std::string nav_label_letter(const std::string& title) { std::string l = letter_of(title); return l.empty() ? std::string("#") : l; }
inline std::string nav_label_year(int year) { return year > 0 ? std::to_string(year) : std::string("?"); }
/* Age buckets for Last played / Recently added: never (t == 0), then Today (< 1 day), This week, This month, Earlier. */
inline std::string nav_label_age(int64_t now, int64_t t, bool never_label = true) {
    if (t <= 0) return never_label ? "Never" : "Unknown";
    const int64_t age = now - t;
    return age < 86400 ? "Today" : age < 7 * 86400 ? "This week" : age < 30 * 86400 ? "This month" : "Earlier";
}
inline std::string nav_label_plays(int64_t seconds) {
    return seconds <= 0 ? "Never" : seconds >= 36000 ? "10h+" : seconds >= 3600 ? "1h+" : "Under 1h";
}
constexpr const char* kNavFavorites = "Favorites";

struct NavSection { std::string label; int first = 0; int count = 0; };
/* Runs of equal labels in list order. An empty label (e.g. the pinned native game) joins the section before it, or the first one after it. */
inline std::vector<NavSection> build_sections(const std::vector<std::string>& labels) {
    std::vector<NavSection> out;
    int lead = 0;   // empty labels at the very start, folded into the first real section
    for (int i = 0; i < (int)labels.size(); ++i) {
        const std::string& l = labels[(size_t)i];
        if (l.empty()) { if (out.empty()) ++lead; else ++out.back().count; continue; }
        if (!out.empty() && out.back().label == l) { ++out.back().count; continue; }
        NavSection s;
        s.label = l; s.first = out.empty() ? 0 : i; s.count = out.empty() ? lead + 1 : 1;
        out.push_back(s);
    }
    return out;
}
inline int section_index(const std::vector<NavSection>& secs, int idx) {
    for (int k = (int)secs.size() - 1; k >= 0; --k) if (idx >= secs[(size_t)k].first) return k;
    return secs.empty() ? -1 : 0;
}
/* LT / RT / PageUp / PageDown: dir > 0 goes to the start of the next section (the last game from the last section); dir < 0 goes to the start of
 * this section, or of the previous one when already there. */
inline int section_step(const std::vector<NavSection>& secs, int cur, int dir, int n) {
    if (n <= 0) return 0;
    cur = clamp_index(cur, n);
    if (secs.empty()) return clamp_index(cur + (dir < 0 ? -8 : 8), n);
    const int k = section_index(secs, cur);
    if (dir > 0) return k + 1 < (int)secs.size() ? secs[(size_t)k + 1].first : n - 1;
    if (cur > secs[(size_t)k].first) return secs[(size_t)k].first;
    return k > 0 ? secs[(size_t)k - 1].first : 0;
}
/* First section with this label (quick-jump by name); -1 when none. */
inline int section_by_label(const std::vector<NavSection>& secs, const std::string& label) {
    for (size_t k = 0; k < secs.size(); ++k) if (secs[k].label == label) return (int)k;
    return -1;
}
/* The scrubber maps the whole list onto its track: fraction 0..1 <-> index 0..n-1. */
inline float scrub_frac(int idx, int n) { return n <= 1 ? 0.f : std::max(0.f, std::min(1.f, (float)idx / (float)(n - 1))); }
inline int scrub_index(float frac, int n) { return n <= 0 ? 0 : clamp_index((int)std::lround(std::max(0.f, std::min(1.f, frac)) * (float)(n - 1)), n); }
/* x pixel of the section's mark on a track [x0, x1]: where its first game sits. */
inline float scrub_x(int idx, int n, float x0, float x1) { return x0 + scrub_frac(idx, n) * (x1 - x0); }
/* "123 / 2,400" */
inline std::string count_text(int pos0, int n) {
    auto group = [](int v) { std::string d = std::to_string(v), o; for (size_t i = 0; i < d.size(); ++i) { if (i && (d.size() - i) % 3 == 0) o += ','; o += d[i]; } return o; };
    return group(n <= 0 ? 0 : pos0 + 1) + " / " + group(n);
}
/* Quick-jump typing (Title sort): the first game whose title starts with `prefix` (case-insensitive); -1 when none. `title_at(i)` returns a
 * const std::string& / const char*. */
template <class F> inline int prefix_index(int n, F&& title_at, const std::string& prefix) {
    if (prefix.empty()) return -1;
    for (int i = 0; i < n; ++i) {
        const std::string t = title_at(i);
        if (t.size() < prefix.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < prefix.size() && ok; ++k) ok = std::tolower((unsigned char)t[k]) == std::tolower((unsigned char)prefix[k]);
        if (ok) return i;
    }
    return -1;
}
/* Held direction: cases per repeat. Slower to start than hold_boost, and it keeps climbing so a 2,000-game library is crossed in about ten seconds. */
inline int nav_hold_boost(float held) { return held < 0.8f ? 1 : held < 1.6f ? 2 : held < 2.6f ? 4 : held < 3.8f ? 8 : held < 5.f ? 14 : 24; }
/* Wheel: notches per second -> cases per notch (1 for a slow scroll, up to 8 for a fast spin). */
inline float nav_wheel_gain(float notches_per_sec) { return 1.f + 7.f * std::max(0.f, std::min(1.f, (notches_per_sec - 5.f) / 15.f)); }
/* True when travel is fast enough that the section bubble should show. */
inline bool nav_is_fast(int hold_boost_now, float wheel_gain_now) { return hold_boost_now >= 2 || wheel_gain_now >= 1.5f; }

/* The sort picker: one row per mode, then the two toggles. */
constexpr int kSortRows = kSortModes + 2;
inline SortSpec sort_apply_row(SortSpec cur, int row) {
    if (row >= 0 && row < kSortModes) { if (cur.mode != (SortMode)row) { cur.mode = (SortMode)row; cur.reverse = false; } }   // a new mode starts in its natural direction; the active one again changes nothing
    else if (row == kSortModes) cur.reverse = !cur.reverse;
    else if (row == kSortModes + 1) cur.favs_first = !cur.favs_first;
    return cur;
}
inline bool sort_row_on(const SortSpec& s, int row) {
    return row < kSortModes ? (int)s.mode == row : row == kSortModes ? s.reverse : s.favs_first;
}

}  // namespace hubui::cases
