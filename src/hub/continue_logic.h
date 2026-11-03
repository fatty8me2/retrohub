#pragma once
/* Home "Continue" hero + "Jump back in" row, pure logic (no ImGui, no files): which game the hero shows, where its backdrop picture comes
 * from, the relative-time wording, the Continue-vs-Play label rules and which recently played games fill the row. Unit tested by
 * hub_continue_test. The UI is continue_ui.cpp. */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hubui::cont {

constexpr int kRecentDays = 30;     // a game counts as "recent" for the Continue hero when played in the last N days
constexpr size_t kRowMax = 6;       // "Jump back in" shows at most this many games

/* One library game as the picker sees it. */
struct Cand {
    std::string key;
    uint64_t last_played = 0;   // unix seconds, 0 = never
    uint64_t play_seconds = 0;
    bool ghost = false;         // a friend's game you do not own: never offered
};

inline bool is_recent(uint64_t now, uint64_t last_played, int days = kRecentDays) {
    if (last_played == 0) return false;
    if (last_played >= now) return true;   // a clock a little ahead of the stamp still counts as just played
    return now - last_played <= (uint64_t)days * 86400ull;
}

/* The game the hero shows by default: the most recently played owned game within `days`; -1 = none (new user: today's hero). Ties keep the
 * earlier entry. */
inline int pick_hero_game(const std::vector<Cand>& g, uint64_t now, int days = kRecentDays) {
    int best = -1;
    for (size_t i = 0; i < g.size(); ++i) {
        if (g[i].ghost || !is_recent(now, g[i].last_played, days)) continue;
        if (best < 0 || g[i].last_played > g[(size_t)best].last_played) best = (int)i;
    }
    return best;
}

/* The "Jump back in" row: up to `n` recently played owned games, newest first (ties keep library order). The hero game is NOT removed:
 * the hero follows the focused tile, so dropping the newest game would make it unreachable once another tile is focused. */
inline std::vector<int> pick_row(const std::vector<Cand>& g, uint64_t now, size_t n = kRowMax, int days = kRecentDays) {
    std::vector<int> v;
    for (size_t i = 0; i < g.size(); ++i)
        if (!g[i].ghost && is_recent(now, g[i].last_played, days)) v.push_back((int)i);
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return g[(size_t)a].last_played > g[(size_t)b].last_played; });
    if (v.size() > n) v.resize(n);
    return v;
}

/* ---- backdrop ---- */
enum class Backdrop { None, StateThumb, Screenshot, Cover };

/* Newest save-state thumbnail wins, then the newest screenshot, then the cover art (blurred/scaled by the caller). */
inline Backdrop pick_backdrop(bool has_state_thumb, bool has_screenshot, bool has_cover) {
    if (has_state_thumb) return Backdrop::StateThumb;
    if (has_screenshot) return Backdrop::Screenshot;
    if (has_cover) return Backdrop::Cover;
    return Backdrop::None;
}

struct Pic {
    std::string path;
    int64_t time = 0;   // unix seconds (file time)
};
/* Index of the newest picture, -1 when there is none. Equal times keep the earlier entry. */
inline int newest_pic(const std::vector<Pic>& p) {
    int best = -1;
    for (size_t i = 0; i < p.size(); ++i)
        if (best < 0 || p[i].time > p[(size_t)best].time) best = (int)i;
    return best;
}

/* ---- wording ---- */

/* "just now" "5 minutes ago" "2 hours ago" "yesterday" "3 days ago" "2 weeks ago" "3 months ago"; "never" for 0. */
inline std::string relative_time(uint64_t now, uint64_t t) {
    if (t == 0) return "never";
    if (t >= now || now - t < 60) return "just now";
    const uint64_t d = now - t;
    auto plural = [](uint64_t n, const char* unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago"; };
    if (d < 3600) return plural(d / 60, "minute");
    if (d < 86400) return plural(d / 3600, "hour");
    const uint64_t days = d / 86400;
    if (days == 1) return "yesterday";
    if (days < 14) return plural(days, "day");
    if (days < 60) return plural(days / 7, "week");
    return plural(days / 30, "month");
}

/* "14 h total" "2.5 h total" "40 min total"; "" under a minute. */
inline std::string total_time(uint64_t secs) {
    if (secs < 60) return "";
    if (secs < 3600) return std::to_string(secs / 60) + " min total";
    char b[32];
    if (secs < 36000) {
        std::snprintf(b, sizeof b, "%.1f h total", (double)(secs / 360) / 10.0);   // truncated to a tenth of an hour
        return b;
    }
    return std::to_string(secs / 3600) + " h total";
}

/* The hero's second line: "Last played 2 hours ago  .  14 h total" (the separator is passed in; the total is dropped when unknown). */
inline std::string last_played_line(uint64_t now, uint64_t last_played, uint64_t play_seconds, const std::string& sep) {
    std::string s = last_played ? "Last played " + relative_time(now, last_played) : std::string("Not played yet");
    const std::string tot = total_time(play_seconds);
    if (!tot.empty()) s += sep + tot;
    return s;
}

/* ---- the hero's buttons ---- */
struct Action {
    std::string label;          // primary button text
    bool resume = false;        // launch loads the auto save state
    bool enabled = true;
    bool show_fresh = false;    // the secondary "Start fresh" button (a plain launch without the state) exists
};

/* supports_states: the runner can load a state at all (retrorun/n64run, not Dolphin/Cemu/Azahar/PCSX2/native). auto_present: auto.state exists.
 * running_this: this game is running now. busy_other: another game / session holds the screen. locked: the lock screen is up.
 * "Continue" only when the runner can actually resume; otherwise honestly "Play". */
inline Action pick_action(bool supports_states, bool auto_present, bool running_this, bool busy_other, bool locked) {
    Action a;
    if (running_this) {
        a.label = "Playing now";
        a.enabled = false;
        return a;
    }
    a.resume = supports_states && auto_present;
    a.label = a.resume ? "Continue" : "Play";
    a.show_fresh = a.resume;
    a.enabled = !busy_other && !locked;
    return a;
}

/* ---- Ken Burns ---- */
/* Slow ping-pong 0..1 (about `period` seconds a leg), eased at both ends so the pan never jerks. */
inline float pingpong01(double t, double period = 28.0) {
    const double u = 0.5 - 0.5 * std::cos(6.283185307179586 * t / (2.0 * period));
    return (float)u;
}

/* Crossfade bookkeeping: the new hero fades in over `secs`; returns the new progress 0..1. */
inline float fade_step(float cur, float dt, float secs = 0.45f) {
    if (secs <= 0.f) return 1.f;
    return std::min(1.f, cur + dt / secs);
}

}  // namespace hubui::cont
