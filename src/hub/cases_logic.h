#pragma once
/* Cases library view (a console-style 3D carousel of game cases): the pure parts. No ImGui here, unit-tested in test_cases.cpp.
 * Slot pose maths (where each case stands for its distance from the centre), the case shapes per console, stepping with wrap / clamp,
 * hold-to-accelerate, the console tab list and its per-console selection memory, the flip / inspect mode machine, and the shuffle pick.
 * The drawing is shelf_draw.cpp (cases section), the integration with the All Games state is libview_ui.cpp. */

#include "libview_logic.h"
#include "shelf_logic.h"
#include "surprise_logic.h"

#include <algorithm>
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
    // x: a Hermite curve to the first neighbour (0 -> 0.80, ends with the shelf pitch), then a tight pitch
    const float x1 = 0.80f, m0 = 1.25f, m1 = 0.16f, pitch = 0.16f;
    float x;
    if (a < 1.f) {
        const float t = a, t2 = t * t, t3 = t2 * t;
        x = (2 * t3 - 3 * t2 + 1) * 0.f + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * x1 + (t3 - t2) * m1;
    } else x = x1 + (a - 1.f) * pitch;
    p.x = sgn * x;
    // yaw: nearly flat at the centre, ~54 degrees for the first neighbour, close to side-on (80 degrees) further out
    const float ymax = 1.42f, k = 1.02f;
    p.yaw = sgn * ymax * (1.f - std::exp(-k * a));
    p.z = 0.55f * std::min(a, 1.f) * (2.f - std::min(a, 1.f)) + 0.11f * std::max(0.f, a - 1.f);
    p.scale = 1.f - 0.05f * std::min(a, 1.f);
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
        default: return CaseKind::Keep;   // GC / Wii U / PS2 / generic: the tall DVD-style keep case
    }
}
inline bool is_box(shelf::Media m) { return kind_for(m) == CaseKind::Box; }
inline bool has_sheen(shelf::Media m) { return !is_box(m); }   // clear plastic is glossy, cardboard is matte
struct CaseDims { float w = 0.71f, h = 1.f, d = 0.075f; };   // world units; a 190 mm tall keep case is 1.0 high
inline CaseDims case_dims(shelf::Media m) {
    using shelf::Media;
    switch (m) {
        case Media::WiiUCase: return {0.71f, 0.92f, 0.075f};   // Blu-ray keep case (135 x 172 mm)
        case Media::Ps1Jewel: return {0.86f, 0.757f, 0.062f};  // jewel case (142 x 125 mm, wider than tall)
        case Media::DsCase: return {0.56f, 0.76f, 0.064f};
        case Media::N3dsCase: return {0.56f, 0.76f, 0.066f};
        case Media::PspCase: return {0.56f, 0.82f, 0.062f};
        case Media::NesCart: return {0.66f, 0.92f, 0.115f};    // cardboard boxes: thicker than the plastic cases
        case Media::SnesCart: return {0.98f, 0.74f, 0.125f};
        case Media::N64Cart: return {0.96f, 0.72f, 0.135f};
        case Media::GbCart: case Media::GbcCart: return {0.62f, 0.80f, 0.095f};
        case Media::GbaCart: return {0.70f, 0.80f, 0.095f};
        default: return {0.71f, 1.f, 0.075f};
    }
}

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
                                 {"psp", "PSP"}, {"nes", "NES"}, {"snes", "SNES"}, {"gba", "GBA"}, {"gbc", "GBC"}, {"gb", "Game Boy"}};
    for (auto& e : m) if (sys == e[0]) return e[1];
    return nullptr;
}
inline int console_rank(const std::string& sys) {
    static const char* o[] = {"n64", "gc", "ps1", "wiiu", "3ds", "nds", "ps2", "psp", "nes", "snes", "gba", "gbc", "gb"};
    for (int i = 0; i < 13; ++i) if (sys == o[i]) return i;
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

}  // namespace hubui::cases
