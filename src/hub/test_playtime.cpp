#include "playtime_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

/* Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil), independent of the code under test. */
static int64_t dfc(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int64_t ts(int y, int m, int d, int h = 0, int mi = 0, int s = 0) { return dfc(y, m, d) * 86400 + h * 3600 + mi * 60 + s; }

int main() {
    using namespace hubui::pt;
    using hub::saves::Session;

    /* ---- group_thousands ---- */
    CHECK(group_thousands(0) == "0");
    CHECK(group_thousands(7) == "7");
    CHECK(group_thousands(999) == "999");
    CHECK(group_thousands(1000) == "1,000");
    CHECK(group_thousands(1234) == "1,234");
    CHECK(group_thousands(999999) == "999,999");
    CHECK(group_thousands(1000000) == "1,000,000");
    CHECK(group_thousands(1234567) == "1,234,567");

    /* ---- hours_text: tenths = (secs + 180) / 360 (min 1); from 1000 tenths: (secs + 1800) / 3600 ---- */
    CHECK(hours_text(0) == "");
    CHECK(hours_text(1) == "0.1 hours");
    CHECK(hours_text(179) == "0.1 hours");
    CHECK(hours_text(180) == "0.1 hours");      // 360 / 360 = 1
    CHECK(hours_text(539) == "0.1 hours");      // 719 / 360 = 1
    CHECK(hours_text(540) == "0.2 hours");      // 720 / 360 = 2
    CHECK(hours_text(1440) == "0.4 hours");     // 1620 / 360 = 4
    CHECK(hours_text(3600) == "1.0 hours");     // 3780 / 360 = 10
    CHECK(hours_text(45000) == "12.5 hours");   // 45180 / 360 = 125
    CHECK(hours_text(153360) == "42.6 hours");  // 153540 / 360 = 426
    CHECK(hours_text(359819) == "99.9 hours");  // 359999 / 360 = 999
    CHECK(hours_text(359820) == "100 hours");   // 360000 / 360 = 1000 -> (361620) / 3600 = 100
    CHECK(hours_text(360000) == "100 hours");
    CHECK(hours_text(1234ull * 3600) == "1,234 hours");
    CHECK(hours_text(1234ull * 3600 + 1799) == "1,234 hours");
    CHECK(hours_text(1234ull * 3600 + 1800) == "1,235 hours");
    CHECK(hours_text(3600ull * 1000000) == "1,000,000 hours");

    /* ---- hours_played / sort_tile_text ---- */
    CHECK(hours_played(0) == "");
    CHECK(hours_played(45000) == "12.5 hours played");
    CHECK(hours_played(1234ull * 3600) == "1,234 hours played");
    CHECK(sort_tile_text(0) == "Not played yet");
    CHECK(sort_tile_text(45000) == "12.5 hours");
    CHECK(sort_tile_text(1) == "0.1 hours");

    /* ---- calendar ---- */
    CHECK(dfc(1970, 1, 1) == 0);
    CHECK(dfc(2024, 2, 29) == 19782);
    CHECK(dfc(2000, 3, 1) == 11017);
    {
        int y = 0, m = 0, d = 0;
        civil_from_day(0, &y, &m, &d);
        CHECK(y == 1970 && m == 1 && d == 1);
        civil_from_day(19782, &y, &m, &d);
        CHECK(y == 2024 && m == 2 && d == 29);
        civil_from_day(19783, &y, &m, &d);
        CHECK(y == 2024 && m == 3 && d == 1);
        civil_from_day(11017, &y, &m, &d);
        CHECK(y == 2000 && m == 3 && d == 1);
        civil_from_day(11016, &y, &m, &d);
        CHECK(y == 2000 && m == 2 && d == 29);      // 2000 is a leap year
        civil_from_day(-1, &y, &m, &d);
        CHECK(y == 1969 && m == 12 && d == 31);
        civil_from_day(dfc(2026, 12, 31), &y, &m, &d);
        CHECK(y == 2026 && m == 12 && d == 31);
        civil_from_day(dfc(1900, 3, 1) - 1, &y, &m, &d);
        CHECK(y == 1900 && m == 2 && d == 28);      // 1900 is not a leap year
    }
    CHECK(local_day(0, 0) == 0);
    CHECK(local_day(86399, 0) == 0);
    CHECK(local_day(86400, 0) == 1);
    CHECK(local_day(-1, 0) == -1);
    CHECK(local_day(-86400, 0) == -1);
    CHECK(local_day(-86401, 0) == -2);
    CHECK(local_day(84600, 3600) == 1);             // 23:30 UTC is already the next day at +1h
    CHECK(local_day(84600, 0) == 0);
    CHECK(local_day(10800, -18000) == -1);          // 03:00 UTC is the previous evening at -5h
    CHECK(local_day(18000, -18000) == 0);
    CHECK(local_day(ts(2024, 2, 29, 12), 0) == 19782);
    CHECK(std::string(month_abbr(1)) == "Jan" && std::string(month_abbr(12)) == "Dec" && std::string(month_abbr(0)).empty() && std::string(month_abbr(13)).empty());

    /* ---- last_played_text ---- */
    {
        const int64_t now = ts(2026, 11, 9, 12);
        CHECK(last_played_text(0, now, 0) == "");
        CHECK(last_played_text(-5, now, 0) == "");
        CHECK(last_played_text(now, now, 0) == "Today");
        CHECK(last_played_text(now - 100, now, 0) == "Today");
        CHECK(last_played_text(ts(2026, 11, 9, 0, 0, 0), now, 0) == "Today");
        CHECK(last_played_text(now + 3 * 86400, now, 0) == "Today");     // another PC's clock ahead
        CHECK(last_played_text(ts(2026, 11, 8, 23, 59, 59), now, 0) == "Yesterday");
        CHECK(last_played_text(ts(2026, 11, 8, 0, 0, 0), now, 0) == "Yesterday");
        CHECK(last_played_text(ts(2026, 11, 7, 23, 59, 59), now, 0) == "Nov 7");
        CHECK(last_played_text(ts(2026, 11, 3, 8), now, 0) == "Nov 3");
        CHECK(last_played_text(ts(2026, 1, 1, 0), now, 0) == "Jan 1");
        CHECK(last_played_text(ts(2025, 12, 31, 23), now, 0) == "Dec 31, 2025");
        CHECK(last_played_text(ts(2025, 11, 9, 12), now, 0) == "Nov 9, 2025");   // exactly a year ago: other year
        CHECK(last_played_text(ts(2024, 2, 29, 12), now, 0) == "Feb 29, 2024");
        CHECK(last_played_text(1, now, 0) == "Jan 1, 1970");
    }
    {   // every month name, same year as now
        const int64_t now = ts(2026, 12, 31, 12);
        const char* mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        for (int m = 1; m <= 12; ++m) CHECK(last_played_text(ts(2026, m, 15, 10), now, 0) == std::string(mn[m - 1]) + " 15");
    }
    {   // timezone moves the day boundary both ways
        const int64_t now = ts(2026, 11, 9, 23, 30);           // UTC Nov 9 23:30
        const int64_t t = ts(2026, 11, 9, 22, 0);
        CHECK(last_played_text(t, now, 0) == "Today");
        CHECK(last_played_text(t, now, 3600) == "Yesterday");   // local: t is Nov 9 23:00, now is Nov 10 00:30
        CHECK(last_played_text(ts(2026, 11, 9, 0, 30), now, 3600) == "Yesterday");
        CHECK(last_played_text(ts(2026, 11, 8, 22, 59), now, 3600) == "Nov 8");   // local Nov 8 23:59, two local days before Nov 10
        const int64_t now2 = ts(2026, 11, 9, 0, 30);          // UTC Nov 9 00:30 = local Nov 8 19:30 at -5h
        const int64_t t2 = ts(2026, 11, 8, 20, 0);            // local Nov 8 15:00
        CHECK(last_played_text(t2, now2, 0) == "Yesterday");
        CHECK(last_played_text(t2, now2, -18000) == "Today");
        CHECK(last_played_text(ts(2026, 11, 8, 3, 0), now2, -18000) == "Yesterday");   // local Nov 7 22:00
        const int64_t ny = ts(2027, 1, 1, 0, 30);              // New Year, UTC
        CHECK(last_played_text(ts(2026, 12, 31, 12), ny, 0) == "Yesterday");
        CHECK(last_played_text(ts(2026, 12, 30, 12), ny, 0) == "Dec 30, 2026");
        CHECK(last_played_text(ts(2026, 12, 31, 12), ny, -3600 * 5) == "Today");      // local Dec 31 19:30 for now
        CHECK(last_played_text(ts(2026, 12, 30, 12), ny, -3600 * 5) == "Yesterday");
    }

    /* ---- header_line ---- */
    {
        const int64_t now = ts(2026, 11, 9, 12);
        const std::string sep = " | ";
        CHECK(header_line(153360, (uint64_t)(now - 100), now, 0, sep) == "42.6 hours played | Last played: Today");
        CHECK(header_line(153360, 0, now, 0, sep) == "42.6 hours played");
        CHECK(header_line(0, (uint64_t)ts(2026, 11, 3, 8), now, 0, sep) == "Last played: Nov 3");
        CHECK(header_line(0, 0, now, 0, sep) == "Not played yet");
        CHECK(header_line(5, (uint64_t)ts(2025, 12, 31, 8), now, 0, sep) == "0.1 hours played | Last played: Dec 31, 2025");
    }

    /* ---- same_pc ---- */
    CHECK(same_pc("PC-A", "pc-a"));
    CHECK(same_pc("PC-A", "PC-A"));
    CHECK(same_pc("", ""));
    CHECK(!same_pc("PC-A", "PC-B"));
    CHECK(!same_pc("a", "ab"));
    CHECK(!same_pc("", "a"));
    CHECK(!same_pc("PC_A", "PC-A"));

    /* ---- game_stats ---- */
    {
        const int64_t now = ts(2026, 11, 9, 12);
        const int64_t from = now - kRecentWindow;
        auto S = [](int64_t start, int64_t secs, const char* dev) { Session s; s.system = "n64"; s.key = "k"; s.start = start; s.secs = secs; s.device = dev; return s; };
        std::vector<Session> v = {
            S(now - 100000, 3600, "pc-a"),      // this PC (case-insensitive), inside the window
            S(now - 50000, 7200, ""),           // empty device = this PC, inside the window
            S(now - 10, 100, "PC-A"),           // runs past now: only 10 s count
            S(from - 1000, 4000, "PC-A"),       // straddles the window start: only 3000 s count
            S(from - 20000, 100, "PC-A"),       // entirely before the window: 0
            S(now - 9000, 0, "PC-A"),           // secs 0: ignored
            S(0, 500, "PC-A"),                  // start 0: ignored
            S(now - 8000, -5, "PC-A"),          // negative secs: ignored
            S(0, 500, "PC-B"),                  // ignored even for another PC
            S(now - 5000, 500, "PC-B"),         // other PCs
            S(now - 4000, 700, "pc-b"),         // same PC as above, other case: one PC
            S(now - 3000, 60, "PC-C"),
        };
        GameStats g = game_stats(20000, (uint64_t)(now - 5), v, "PC-A", now);
        CHECK(g.sessions == 5);
        CHECK(g.longest_secs == 7200);
        CHECK(g.recent_secs == 3600 + 7200 + 10 + 3000);
        CHECK(g.other_secs == 500 + 700 + 60);
        CHECK(g.other_pcs == 2);
        CHECK(g.total_secs == 20000);                         // library.json bigger than the log: library wins
        CHECK(g.last_played == (uint64_t)(now - 5));          // library.json newer than the newest session (now - 10)
        CHECK(g.played());
        g = game_stats(0, 0, v, "PC-A", now);                 // library.json younger than the vault: the log wins
        CHECK(g.total_secs == 3600 + 7200 + 100 + 4000 + 100);
        CHECK(g.last_played == (uint64_t)(now - 10));
        CHECK(g.played());
        g = game_stats(100, 5, v, "PC-A", now);
        CHECK(g.total_secs == 15000 && g.last_played == (uint64_t)(now - 10));
        g = game_stats(15000, 0, v, "pc-a", now);             // this_pc compares case-insensitively; equal totals
        CHECK(g.total_secs == 15000 && g.sessions == 5 && g.other_pcs == 2);
        g = game_stats(0, 0, v, "PC-B", now);                 // seen from the other PC: PC-B's two sessions are "this PC" ... plus the empty-device one
        CHECK(g.sessions == 3 && g.total_secs == 7200 + 500 + 700);   // the empty-device session counts as this PC
        CHECK(g.other_pcs == 2 && g.other_secs == 3600 + 100 + 4000 + 100 + 60);   // PC-A (4 sessions) and PC-C
        g = game_stats(0, 0, v, "PC-A", now, 86400);          // a one-day window
        CHECK(g.recent_secs == 7200 + 10);                    // the 100000 s old session ended 96400 s ago: out
        g = game_stats(0, 0, v, "PC-A", now, 1000000000);     // a huge window clips nothing
        CHECK(g.recent_secs == 3600 + 7200 + 10 + 4000 + 100);
    }
    {
        const int64_t now = ts(2026, 11, 9, 12);
        std::vector<Session> none;
        GameStats g = game_stats(5, 0, none, "PC-A", now);
        CHECK(g.total_secs == 5 && g.last_played == 0 && g.sessions == 0 && g.recent_secs == 0 && g.longest_secs == 0 && g.other_secs == 0 && g.other_pcs == 0 && g.played());
        g = game_stats(0, 0, none, "PC-A", now);
        CHECK(!g.played());
        g = game_stats(0, 77, none, "PC-A", now);
        CHECK(g.played() && g.total_secs == 0 && g.last_played == 77);
        Session o; o.start = now - 100; o.secs = 100; o.device = "PC-B";
        g = game_stats(0, 0, {o}, "PC-A", now);               // only other PCs: never added to this PC's numbers
        CHECK(!g.played() && g.total_secs == 0 && g.last_played == 0 && g.sessions == 0 && g.other_secs == 100 && g.other_pcs == 1);
        g = game_stats(3600, (uint64_t)now, {o}, "PC-A", now);
        CHECK(g.total_secs == 3600 && g.other_secs == 100);
        CHECK(!GameStats().played());
    }

    /* ---- strip_cells ---- */
    {
        const int64_t now = ts(2026, 11, 9, 12);
        auto S = [](int64_t start, int64_t secs, const char* dev) { Session s; s.start = start; s.secs = secs; s.device = dev; return s; };
        std::vector<Session> v = {S(now - 100000, 3600, "PC-A"), S(now - 50000, 7200, "PC-A"), S(now - 10, 100, "PC-A"), S(now - 5000, 500, "PC-B"),
                                  S(now - 4000, 700, "pc-b"), S(now - 3000, 60, "PC-C")};
        GameStats g = game_stats(20000, (uint64_t)(now - 5), v, "PC-A", now);
        // recent = 3600 + 7200 + 10 = 10810 s -> (10990 / 360 = 30) "3.0 hours"; other = 1260 s -> 1440 / 360 = 4 "0.4 hours"; total 20000 s -> 20180 / 360 = 56 "5.6 hours"
        std::vector<Cell> c = strip_cells(g, now, 0);
        CHECK(c.size() == 5);
        CHECK(c.size() == 5 && c[0].label == "Play time" && c[0].value == "5.6 hours");
        CHECK(c.size() == 5 && c[1].label == "Last played" && c[1].value == "Today");
        CHECK(c.size() == 5 && c[2].label == "Sessions" && c[2].value == "3");
        CHECK(c.size() == 5 && c[3].label == "Last 2 weeks" && c[3].value == "3.0 hours");
        CHECK(c.size() == 5 && c[4].label == "Other PCs" && c[4].value == "0.4 hours");

        std::vector<Session> one = {S(now - 100, 3600, "PC-B")};
        c = strip_cells(game_stats(3600, (uint64_t)(now - 86400), one, "PC-A", now), now, 0);   // one other PC, no own sessions
        CHECK(c.size() == 3);
        CHECK(c.size() == 3 && c[0].label == "Play time" && c[0].value == "1.0 hours");
        CHECK(c.size() == 3 && c[1].label == "Last played" && c[1].value == "Yesterday");
        CHECK(c.size() == 3 && c[2].label == "Other PC" && c[2].value == "1.0 hours");

        CHECK(strip_cells(GameStats(), now, 0).empty());                       // never played
        GameStats only_other; only_other.other_secs = 5000; only_other.other_pcs = 1;
        CHECK(strip_cells(only_other, now, 0).empty());                        // never played here: nothing, even with other-PC time

        GameStats a; a.total_secs = 90000;                                      // total only: no date, sessions, recent
        c = strip_cells(a, now, 0);
        CHECK(c.size() == 1 && c[0].label == "Play time" && c[0].value == "25.0 hours");
        GameStats b; b.last_played = (uint64_t)ts(2026, 11, 7, 12);             // date only: no play time cell
        c = strip_cells(b, now, 0);
        CHECK(c.size() == 1 && c[0].label == "Last played" && c[0].value == "Nov 7");
        GameStats d; d.total_secs = 1234ull * 3600; d.last_played = (uint64_t)ts(2025, 12, 31, 12); d.sessions = 1234; d.recent_secs = 7200; d.other_secs = 3600 * 100; d.other_pcs = 3;
        c = strip_cells(d, now, 0);
        CHECK(c.size() == 5);
        CHECK(c.size() == 5 && c[0].value == "1,234 hours" && c[1].value == "Dec 31, 2025" && c[2].value == "1,234" && c[3].value == "2.0 hours" && c[4].label == "Other PCs" && c[4].value == "100 hours");
        d.sessions = 0; d.recent_secs = 0;                                      // sessions / recent drop out when zero
        c = strip_cells(d, now, 0);
        CHECK(c.size() == 3 && c[0].label == "Play time" && c[1].label == "Last played" && c[2].label == "Other PCs");
    }

    std::printf(fails ? "hub_playtime_test: %d FAILED\n" : "hub_playtime_test: all ok\n", fails);
    return fails;
}
