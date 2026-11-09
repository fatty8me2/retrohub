#pragma once
/* Steam-style play time for the library and the game page (pure: no ImGui, no files, no clock). Unit tested by hub_playtime_test.
 *
 * No new storage. Two existing sources:
 *   - library.json (hub::GameState): this PC's lifetime total (play_seconds) and last played. The "Most played" sort, the tiles and the
 *     list rows use exactly these numbers.
 *   - the vault play log (playstats.h): one "<start>,<seconds>" line per finished session in <game dir>/playlog/<PC>.log, one file per
 *     PC (the cloud saves bring the other PCs' files here). playlog_read_game() reads one game's files.
 * The game page counts every PC, like Steam's account-wide hours: this PC's total plus the time other PCs logged. When another PC
 * logged time, the strip also shows the "This PC" part (the tile's number) and the "Other PC(s)" part, so the two always add up. */

#include "playstats.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hubui::pt {

constexpr int64_t kRecentWindow = 14 * 86400;   // Steam's "last two weeks"

/* "7", "1,234", "1,000,000". */
inline std::string group_thousands(uint64_t n) {
    const std::string d = std::to_string(n);
    std::string o;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) o.push_back(',');
        o.push_back(d[i]);
    }
    return o;
}

/* Steam's hours. "" for 0 seconds. Under 100 hours: one decimal, rounded to the nearest tenth (halves up) and never below "0.1 hours":
 * "0.1 hours", "0.4 hours", "12.5 hours", "99.9 hours". From 100 hours (after that rounding): whole hours, rounded, with thousands
 * separators: "100 hours", "1,234 hours". Always "hours" (Steam says "1.0 hours"). */
inline std::string hours_text(uint64_t secs) {
    if (secs == 0) return "";
    uint64_t tenths = (secs + 180) / 360;
    if (tenths < 1) tenths = 1;
    if (tenths < 1000) return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " hours";
    return group_thousands((secs + 1800) / 3600) + " hours";
}

/* "42.6 hours played"; "" when never played. */
inline std::string hours_played(uint64_t secs) {
    const std::string h = hours_text(secs);
    return h.empty() ? h : h + " played";
}

/* Tile / list-row text while the "Most played" sort is on: "12.5 hours", or "Not played yet". */
inline std::string sort_tile_text(uint64_t secs) { return secs ? hours_text(secs) : std::string("Not played yet"); }

/* ---- calendar (proleptic Gregorian, no locale / mktime; tz = local time minus UTC, seconds) ---- */
inline int64_t local_day(int64_t t, int tz) {
    const int64_t x = t + tz;
    return x >= 0 ? x / 86400 : -((-x + 86399) / 86400);
}
inline void civil_from_day(int64_t z, int* y, int* m, int* d) {   // days since 1970-01-01 -> year, month 1-12, day 1-31
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}
inline const char* month_abbr(int m) {
    static const char* n[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return m >= 1 && m <= 12 ? n[m - 1] : "";
}

/* Steam's "Last played" value, by local calendar day: "Today", "Yesterday", "Nov 3" (the same year as now), "Nov 3, 2025" (another
 * year); "" for 0 (never). A stamp later than now (another PC's clock) says "Today". */
inline std::string last_played_text(int64_t t, int64_t now, int tz) {
    if (t <= 0) return "";
    const int64_t dt = local_day(t, tz), dn = local_day(now, tz);
    if (dt >= dn) return "Today";
    if (dt == dn - 1) return "Yesterday";
    int y = 0, m = 0, d = 0, ny = 0, nm = 0, nd = 0;
    civil_from_day(dt, &y, &m, &d);
    civil_from_day(dn, &ny, &nm, &nd);
    std::string s = std::string(month_abbr(m)) + " " + std::to_string(d);
    if (y != ny) s += ", " + std::to_string(y);
    return s;
}

/* The game page's header line: "42.6 hours played<sep>Last played: Nov 3". Either half alone when the other is unknown;
 * "Not played yet" when both are. */
inline std::string header_line(uint64_t secs, uint64_t last_played, int64_t now, int tz, const std::string& sep) {
    const std::string h = hours_played(secs), l = last_played_text((int64_t)last_played, now, tz);
    if (h.empty() && l.empty()) return "Not played yet";
    if (l.empty()) return h;
    if (h.empty()) return "Last played: " + l;
    return h + sep + "Last played: " + l;
}

/* ---- one game's numbers ---- */
struct GameStats {
    uint64_t total_secs = 0;     // every PC: this_pc_secs + other_secs
    uint64_t this_pc_secs = 0;   // this PC: library.json's total, or this PC's logged sessions when they add up to more (library.json younger than the vault)
    uint64_t other_secs = 0;     // the time other PCs logged
    int other_pcs = 0;           // how many other PCs logged time for this game
    uint64_t last_played = 0;    // every PC: unix seconds, the newest of library.json's last_played and every logged session start; 0 = never
    int sessions = 0;            // logged sessions, every PC
    int64_t recent_secs = 0;     // every PC: time played inside [now - window, now] (only the part of a session inside the window counts)
    int64_t longest_secs = 0;    // every PC: the longest logged session
    bool played() const { return total_secs > 0 || last_played > 0; }
};

/* PC names compare case-insensitively (Windows computer names are). */
inline bool same_pc(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

/* lib_total / lib_last: library.json (hub::GameState play_seconds / last_played, this PC). sessions: the game's play log from every PC
 * (hub::saves::playlog_read_game); this_pc: hub::saves::playlog_device(). A session with an empty device counts as this PC.
 * Sessions with secs <= 0 or start <= 0 are ignored. */
inline GameStats game_stats(uint64_t lib_total, uint64_t lib_last, const std::vector<hub::saves::Session>& sessions, const std::string& this_pc,
                            int64_t now, int64_t window = kRecentWindow) {
    GameStats g;
    uint64_t logged_here = 0;
    int64_t newest = 0;
    std::vector<std::string> others;
    const int64_t from = now - window;
    for (const hub::saves::Session& s : sessions) {
        if (s.secs <= 0 || s.start <= 0) continue;
        if (!s.device.empty() && !same_pc(s.device, this_pc)) {
            g.other_secs += (uint64_t)s.secs;
            if (std::none_of(others.begin(), others.end(), [&](const std::string& o) { return same_pc(o, s.device); })) others.push_back(s.device);
        } else {
            logged_here += (uint64_t)s.secs;
        }
        g.sessions++;
        newest = std::max(newest, s.start);
        g.longest_secs = std::max(g.longest_secs, s.secs);
        const int64_t a = std::max(s.start, from), b = std::min(s.start + s.secs, now);
        if (b > a) g.recent_secs += b - a;
    }
    g.other_pcs = (int)others.size();
    g.this_pc_secs = std::max(lib_total, logged_here);
    g.total_secs = g.this_pc_secs + g.other_secs;
    g.last_played = std::max(lib_last, (uint64_t)newest);
    return g;
}

/* ---- the stats strip under the header ---- */
struct Cell { std::string label, value; };

/* In order: "Play time" (every PC, when > 0), "Last played" (when known), "Sessions" (when any were logged), "Last 2 weeks" (when > 0),
 * then, only when another PC logged time: "This PC" (when > 0) and "Other PC" / "Other PCs" (one / several other PCs).
 * Empty when the game was never played. Values: hours_text / last_played_text / group_thousands. */
inline std::vector<Cell> strip_cells(const GameStats& g, int64_t now, int tz) {
    std::vector<Cell> v;
    if (!g.played()) return v;
    if (g.total_secs) v.push_back({"Play time", hours_text(g.total_secs)});
    const std::string lp = last_played_text((int64_t)g.last_played, now, tz);
    if (!lp.empty()) v.push_back({"Last played", lp});
    if (g.sessions > 0) v.push_back({"Sessions", group_thousands((uint64_t)g.sessions)});
    if (g.recent_secs > 0) v.push_back({"Last 2 weeks", hours_text((uint64_t)g.recent_secs)});
    if (g.other_secs > 0) {
        if (g.this_pc_secs) v.push_back({"This PC", hours_text(g.this_pc_secs)});
        v.push_back({g.other_pcs == 1 ? "Other PC" : "Other PCs", hours_text(g.other_secs)});
    }
    return v;
}

}  // namespace hubui::pt
