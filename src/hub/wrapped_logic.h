#pragma once
/* RetroHub Wrapped (Wave 3 L8), pure logic: month / year ranges and the aggregation of the play history into the story cards.
 *
 * Input is the same data the "Your stats" card uses: the per-session play log (hub::saves::Session) plus the recent online
 * groups (group_logic.h) for the most-played-with friend. Nothing here touches files or the UI. */

#include "playstats.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hubui::wrapped {

enum class Period { Month, Year };

struct Range {
    int64_t from = 0, to = 0;   // unix seconds, [from, to)
    int year = 0, month = 0;    // month 1-12 (Month only; 0 for a year)
};

/* ---- calendar helpers (proleptic Gregorian, no locale / mktime) ---- */
inline int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
inline void civil_from_days(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

/* The calendar month / year containing `now` (local time = UTC + tz seconds), `back` months / years earlier. */
inline Range period_range(Period p, int64_t now, int tz, int back = 0) {
    int y, m, d;
    civil_from_days(floor_div(now + tz, 86400), &y, &m, &d);
    Range r;
    if (p == Period::Year) {
        y -= back;
        r.year = y;
        r.month = 0;
        r.from = days_from_civil(y, 1, 1) * 86400 - tz;
        r.to = days_from_civil(y + 1, 1, 1) * 86400 - tz;
    } else {
        int idx = y * 12 + (m - 1) - back;
        y = idx / 12;
        m = idx % 12 + 1;
        r.year = y;
        r.month = m;
        r.from = days_from_civil(y, m, 1) * 86400 - tz;
        r.to = (m == 12 ? days_from_civil(y + 1, 1, 1) : days_from_civil(y, m + 1, 1)) * 86400 - tz;
    }
    return r;
}

inline const char* month_name(int m) {
    static const char* n[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    return m >= 1 && m <= 12 ? n[m - 1] : "";
}
inline std::string period_label(const Range& r) {
    return r.month ? std::string(month_name(r.month)) + " " + std::to_string(r.year) : std::to_string(r.year);
}

/* ---- aggregation ---- */
struct GameSecs { std::string system, key; int64_t secs = 0; int sessions = 0; };

/* One finished online group (RecentGroup) reduced to what Wrapped needs. */
struct GroupEvent {
    int64_t at_s = 0;                                      // when it ended, unix seconds
    std::vector<std::pair<std::string, std::string>> members;   // uid, display name (everyone but us)
};

struct FriendStat { std::string uid, name; int together = 0; };

struct Data {
    Period period = Period::Month;
    Range range;
    int64_t total_secs = 0;
    int sessions = 0;
    int active_days = 0;
    int games_played = 0;
    std::vector<GameSecs> top;                                  // up to 5, most played first
    std::vector<std::pair<std::string, int64_t>> systems;       // system short -> secs, biggest first
    int longest_streak = 0;                                     // consecutive days with play inside the range
    int64_t longest_secs = 0, longest_start = 0;                // longest single session
    int busiest_weekday = -1;                                   // 0 = Sunday .. 6; -1 = none
    int64_t busiest_weekday_secs = 0;
    bool has_friend = false;
    FriendStat best_friend;
    bool empty() const { return sessions == 0; }
};

inline int weekday_of_day(int64_t day) { return (int)(((day % 7) + 11) % 7); }   // day 0 = 1970-01-01 = Thursday(4); 0 = Sunday

inline Data aggregate(const std::vector<hub::saves::Session>& sessions, const std::vector<GroupEvent>& groups, Period period,
                      const Range& range, int tz, size_t top_n = 5) {
    Data d;
    d.period = period;
    d.range = range;
    std::map<std::pair<std::string, std::string>, GameSecs> per_game;
    std::map<std::string, int64_t> per_sys;
    std::set<int64_t> days;
    std::map<int, int64_t> per_wd;
    for (const auto& s : sessions) {
        if (s.secs <= 0 || s.start < range.from || s.start >= range.to) continue;
        d.total_secs += s.secs;
        d.sessions++;
        GameSecs& g = per_game[{s.system, s.key}];
        g.system = s.system;
        g.key = s.key;
        g.secs += s.secs;
        g.sessions++;
        per_sys[s.system] += s.secs;
        const int64_t day = floor_div(s.start + tz, 86400);
        days.insert(day);
        per_wd[weekday_of_day(day)] += s.secs;
        if (s.secs > d.longest_secs) { d.longest_secs = s.secs; d.longest_start = s.start; }
    }
    d.active_days = (int)days.size();
    d.games_played = (int)per_game.size();
    for (auto& kv : per_game) d.top.push_back(kv.second);
    std::stable_sort(d.top.begin(), d.top.end(), [](const GameSecs& a, const GameSecs& b) { return a.secs != b.secs ? a.secs > b.secs : a.key < b.key; });
    if (d.top.size() > top_n) d.top.resize(top_n);
    for (auto& kv : per_sys) d.systems.push_back(kv);
    std::stable_sort(d.systems.begin(), d.systems.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    int run = 0;
    int64_t prev = 0;
    for (int64_t day : days) {   // std::set iterates ascending
        run = (run > 0 && day == prev + 1) ? run + 1 : 1;
        prev = day;
        d.longest_streak = std::max(d.longest_streak, run);
    }
    for (auto& kv : per_wd)
        if (kv.second > d.busiest_weekday_secs) { d.busiest_weekday_secs = kv.second; d.busiest_weekday = kv.first; }

    /* most-played-with friend: how many of the recent online groups inside the range each friend was part of; ties go to the newest */
    struct Acc { std::string name; int n = 0; int64_t last = 0; };
    std::map<std::string, Acc> fr;
    for (const auto& g : groups) {
        if (g.at_s < range.from || g.at_s >= range.to) continue;
        std::set<std::string> seen;   // a friend counts once per group
        for (const auto& m : g.members) {
            if (m.first.empty() || !seen.insert(m.first).second) continue;
            Acc& a = fr[m.first];
            a.n++;
            if (g.at_s >= a.last) { a.last = g.at_s; a.name = m.second; }
        }
    }
    int64_t best_last = -1;
    for (auto& kv : fr) {
        if (kv.second.n > d.best_friend.together || (kv.second.n == d.best_friend.together && kv.second.last > best_last)) {
            d.best_friend = {kv.first, kv.second.name, kv.second.n};
            best_last = kv.second.last;
            d.has_friend = true;
        }
    }
    return d;
}

inline const char* weekday_name(int wd) {
    static const char* n[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return wd >= 0 && wd <= 6 ? n[wd] : "";
}

/* "12 h 30 min" style text for the cards. */
inline std::string hours_text(int64_t secs) {
    if (secs <= 0) return "0 min";
    if (secs < 60) return "<1 min";
    const int64_t mins = secs / 60;
    if (mins < 60) return std::to_string(mins) + " min";
    const int64_t h = mins / 60, m = mins % 60;
    return m ? std::to_string(h) + " h " + std::to_string(m) + " min" : std::to_string(h) + " h";
}

}  // namespace hubui::wrapped
