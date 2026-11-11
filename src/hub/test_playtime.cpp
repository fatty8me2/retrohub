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

    /* ---- duration_text / hours_text (long) / hours_compact: the one play-time formatter ---- */
    struct Row { uint64_t secs; const char* compact; const char* lng; };
    const Row table[] = {
        {0, "", ""},
        {1, "<1 min", "Under a minute"},
        {59, "<1 min", "Under a minute"},
        {60, "1 min", "1 minute"},
        {119, "1 min", "1 minute"},
        {120, "2 min", "2 minutes"},
        {59 * 60, "59 min", "59 minutes"},
        {3599, "59 min", "59 minutes"},
        {3600, "1 h", "1 hour"},                     // never "1.0"
        {3779, "1 h", "1 hour"},                     // 3959 / 360 = 10 tenths
        {3780, "1.1 h", "1.1 hours"},
        {5400, "1.5 h", "1.5 hours"},
        {7200, "2 h", "2 hours"},
        {27000, "7.5 h", "7.5 hours"},
        {35640, "9.9 h", "9.9 hours"},               // 9.9 h
        {35819, "9.9 h", "9.9 hours"},               // 35999 / 360 = 99 tenths
        {35820, "10 h", "10 hours"},                 // 36000 / 360 = 100 tenths: whole hours from here
        {36000, "10 h", "10 hours"},
        {99 * 3600 + 1800, "100 h", "100 hours"},    // 99.5 h rounds up
        {100 * 3600, "100 h", "100 hours"},
        {45000, "13 h", "13 hours"},                 // 12.5 h: halves up
        {1000ull * 3600, "1,000 h", "1,000 hours"},
        {1204ull * 3600, "1,204 h", "1,204 hours"},
        {12345ull * 3600, "12,345 h", "12,345 hours"},
        {12345ull * 3600 + 1799, "12,345 h", "12,345 hours"},
        {12345ull * 3600 + 1800, "12,346 h", "12,346 hours"},
    };
    for (const Row& r : table) {
        CHECK(duration_text(r.secs, true) == r.compact);
        CHECK(duration_text(r.secs, false) == r.lng);
        CHECK(hours_compact(r.secs) == r.compact);
        CHECK(hours_text(r.secs) == r.lng);
    }

    /* ---- hours_played / sort_tile_text ---- */
    CHECK(hours_played(0) == "");
    CHECK(hours_played(45000) == "13 hours played");
    CHECK(hours_played(27000) == "7.5 hours played");
    CHECK(hours_played(3600) == "1 hour played");
    CHECK(hours_played(1234ull * 3600) == "1,234 hours played");
    CHECK(sort_tile_text(0) == "Not played yet");
    CHECK(sort_tile_text(27000) == "7.5 h");
    CHECK(sort_tile_text(1) == "<1 min");

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
        CHECK(header_line(153360, (uint64_t)(now - 100), now, 0, sep) == "43 hours played | Last played: Today");
        CHECK(header_line(153360, 0, now, 0, sep) == "43 hours played");
        CHECK(header_line(0, (uint64_t)ts(2026, 11, 3, 8), now, 0, sep) == "Last played: Nov 3");
        CHECK(header_line(0, 0, now, 0, sep) == "Not played yet");
        CHECK(header_line(5, (uint64_t)ts(2025, 12, 31, 8), now, 0, sep) == "Under a minute played | Last played: Dec 31, 2025");
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
        CHECK(g.sessions == 5 + 3);                           // every PC's valid sessions: 5 here, 3 elsewhere
        CHECK(g.longest_secs == 7200);
        CHECK(g.recent_secs == 3600 + 7200 + 10 + 3000 + 500 + 700 + 60);
        CHECK(g.other_secs == 500 + 700 + 60);
        CHECK(g.other_pcs == 2);
        CHECK(g.this_pc_secs == 20000);                       // library.json bigger than this PC's log: library wins
        CHECK(g.total_secs == 20000 + 1260);                  // every PC: this PC + the others
        CHECK(g.last_played == (uint64_t)(now - 5));          // library.json newer than the newest session (now - 10)
        CHECK(g.played());
        g = game_stats(0, 0, v, "PC-A", now);                 // library.json younger than the vault: this PC's log wins
        CHECK(g.this_pc_secs == 3600 + 7200 + 100 + 4000 + 100);
        CHECK(g.total_secs == 15000 + 1260);
        CHECK(g.last_played == (uint64_t)(now - 10));         // newest start of any PC's session
        CHECK(g.played());
        g = game_stats(100, 5, v, "PC-A", now);
        CHECK(g.this_pc_secs == 15000 && g.total_secs == 16260 && g.last_played == (uint64_t)(now - 10));
        g = game_stats(15000, 0, v, "pc-a", now);             // this_pc compares case-insensitively; equal totals
        CHECK(g.this_pc_secs == 15000 && g.total_secs == 16260 && g.sessions == 8 && g.other_pcs == 2);
        g = game_stats(0, 0, v, "PC-B", now);                 // seen from PC-B: its two sessions plus the empty-device one are "this PC"
        CHECK(g.this_pc_secs == 7200 + 500 + 700);
        CHECK(g.other_pcs == 2 && g.other_secs == 3600 + 100 + 4000 + 100 + 60);   // PC-A (4 sessions) and PC-C
        CHECK(g.total_secs == 16260 && g.sessions == 8);      // the account-wide total is the same from every PC
        g = game_stats(0, 0, v, "PC-A", now, 86400);          // a one-day window
        CHECK(g.recent_secs == 7200 + 10 + 500 + 700 + 60);   // the 100000 s old session ended 96400 s ago: out
        g = game_stats(0, 0, v, "PC-A", now, 1000000000);     // a huge window clips nothing
        CHECK(g.recent_secs == 3600 + 7200 + 10 + 4000 + 100 + 1260);
    }
    {
        const int64_t now = ts(2026, 11, 9, 12);
        std::vector<Session> none;
        GameStats g = game_stats(5, 0, none, "PC-A", now);
        CHECK(g.total_secs == 5 && g.this_pc_secs == 5 && g.last_played == 0 && g.sessions == 0 && g.recent_secs == 0 && g.longest_secs == 0 && g.other_secs == 0 && g.other_pcs == 0 && g.played());
        g = game_stats(0, 0, none, "PC-A", now);
        CHECK(!g.played());
        g = game_stats(0, 77, none, "PC-A", now);
        CHECK(g.played() && g.total_secs == 0 && g.last_played == 77);
        Session o; o.start = now - 100; o.secs = 100; o.device = "PC-B";
        g = game_stats(0, 0, {o}, "PC-A", now);               // played only on another PC: it still counts for the game
        CHECK(g.played() && g.total_secs == 100 && g.this_pc_secs == 0 && g.last_played == (uint64_t)(now - 100) && g.sessions == 1 && g.other_secs == 100 && g.other_pcs == 1);
        CHECK(g.recent_secs == 100 && g.longest_secs == 100);
        g = game_stats(3600, (uint64_t)now, {o}, "PC-A", now);
        CHECK(g.this_pc_secs == 3600 && g.total_secs == 3700 && g.other_secs == 100 && g.last_played == (uint64_t)now);
        CHECK(!GameStats().played());
    }

    /* ---- strip_cells ---- */
    {
        const int64_t now = ts(2026, 11, 9, 12);
        auto S = [](int64_t start, int64_t secs, const char* dev) { Session s; s.start = start; s.secs = secs; s.device = dev; return s; };
        std::vector<Session> v = {S(now - 100000, 3600, "PC-A"), S(now - 50000, 7200, "PC-A"), S(now - 10, 100, "PC-A"), S(now - 5000, 500, "PC-B"),
                                  S(now - 4000, 700, "pc-b"), S(now - 3000, 60, "PC-C")};
        GameStats g = game_stats(20000, (uint64_t)(now - 5), v, "PC-A", now);
        // total = 20000 + 1260 = 21260 s -> "5.9 hours"; recent = 3600 + 7200 + 10 + 1260 = 12070 s -> "3.4 hours";
        // this PC 20000 s -> "5.6 hours"; other 1260 s -> "21 minutes"
        std::vector<Cell> c = strip_cells(g, now, 0);
        CHECK(c.size() == 6);
        CHECK(c.size() == 6 && c[0].label == "Play time" && c[0].value == "5.9 hours");
        CHECK(c.size() == 6 && c[1].label == "Last played" && c[1].value == "Today");
        CHECK(c.size() == 6 && c[2].label == "Sessions" && c[2].value == "6");
        CHECK(c.size() == 6 && c[3].label == "Last 2 weeks" && c[3].value == "3.4 hours");
        CHECK(c.size() == 6 && c[4].label == "This PC" && c[4].value == "5.6 hours");
        CHECK(c.size() == 6 && c[5].label == "Other PCs" && c[5].value == "21 minutes");

        std::vector<Session> one = {S(now - 100, 3600, "PC-B")};
        c = strip_cells(game_stats(3600, (uint64_t)(now - 86400), one, "PC-A", now), now, 0);   // one other PC
        // total 7200 s -> "2 hours"; recent 100 s -> "1 minute"
        CHECK(c.size() == 6);
        CHECK(c.size() == 6 && c[0].label == "Play time" && c[0].value == "2 hours");
        CHECK(c.size() == 6 && c[1].label == "Last played" && c[1].value == "Today");   // the other PC's session is newer than library.json's stamp
        CHECK(c.size() == 6 && c[2].label == "Sessions" && c[2].value == "1");
        CHECK(c.size() == 6 && c[3].label == "Last 2 weeks" && c[3].value == "1 minute");
        CHECK(c.size() == 6 && c[4].label == "This PC" && c[4].value == "1 hour");
        CHECK(c.size() == 6 && c[5].label == "Other PC" && c[5].value == "1 hour");

        {   // played only on another PC (library.json 0 / 0): no "This PC" cell
            std::vector<Session> lap = {S(now - 3 * 86400, 5400, "LAPTOP")};   // Nov 6 12:00, 1.5 h
            GameStats lg = game_stats(0, 0, lap, "PC-A", now);
            CHECK(lg.played() && lg.total_secs == 5400 && lg.this_pc_secs == 0);
            c = strip_cells(lg, now, 0);
            CHECK(c.size() == 5);
            CHECK(c.size() == 5 && c[0].label == "Play time" && c[0].value == "1.5 hours");   // 5580 / 360 = 15
            CHECK(c.size() == 5 && c[1].label == "Last played" && c[1].value == "Nov 6");
            CHECK(c.size() == 5 && c[2].label == "Sessions" && c[2].value == "1");
            CHECK(c.size() == 5 && c[3].label == "Last 2 weeks" && c[3].value == "1.5 hours");
            CHECK(c.size() == 5 && c[4].label == "Other PC" && c[4].value == "1.5 hours");
            CHECK(header_line(lg.total_secs, lg.last_played, now, 0, " | ") == "1.5 hours played | Last played: Nov 6");
            lap = {S(now - 30 * 86400, 5400, "LAPTOP")};                              // Oct 10: outside the two weeks
            c = strip_cells(game_stats(0, 0, lap, "PC-A", now), now, 0);
            CHECK(c.size() == 4);
            CHECK(c.size() == 4 && c[0].label == "Play time" && c[1].label == "Last played" && c[1].value == "Oct 10" && c[2].label == "Sessions" && c[3].label == "Other PC");
        }

        CHECK(strip_cells(GameStats(), now, 0).empty());                       // never played
        GameStats only_other; only_other.other_secs = 5000; only_other.other_pcs = 1;
        CHECK(strip_cells(only_other, now, 0).empty());                        // never played here: nothing, even with other-PC time

        GameStats a; a.total_secs = 90000;                                      // total only: no date, sessions, recent
        c = strip_cells(a, now, 0);
        CHECK(c.size() == 1 && c[0].label == "Play time" && c[0].value == "25 hours");
        GameStats b; b.last_played = (uint64_t)ts(2026, 11, 7, 12);             // date only: no play time cell
        c = strip_cells(b, now, 0);
        CHECK(c.size() == 1 && c[0].label == "Last played" && c[0].value == "Nov 7");
        GameStats d; d.total_secs = 1234ull * 3600; d.this_pc_secs = 1134ull * 3600; d.last_played = (uint64_t)ts(2025, 12, 31, 12); d.sessions = 1234; d.recent_secs = 7200; d.other_secs = 3600 * 100; d.other_pcs = 3;
        c = strip_cells(d, now, 0);
        CHECK(c.size() == 6);
        CHECK(c.size() == 6 && c[0].value == "1,234 hours" && c[1].value == "Dec 31, 2025" && c[2].value == "1,234" && c[3].value == "2 hours");
        CHECK(c.size() == 6 && c[4].label == "This PC" && c[4].value == "1,134 hours" && c[5].label == "Other PCs" && c[5].value == "100 hours");
        d.sessions = 0; d.recent_secs = 0;                                      // sessions / recent drop out when zero
        c = strip_cells(d, now, 0);
        CHECK(c.size() == 4 && c[0].label == "Play time" && c[1].label == "Last played" && c[2].label == "This PC" && c[3].label == "Other PCs");
        d.this_pc_secs = 0;                                                     // nothing played here: no "This PC"
        c = strip_cells(d, now, 0);
        CHECK(c.size() == 3 && c[2].label == "Other PCs");
        d.other_secs = 0;                                                       // no other PC: no "This PC" / "Other PC" at all
        c = strip_cells(d, now, 0);
        CHECK(c.size() == 2 && c[0].label == "Play time" && c[1].label == "Last played");
    }

    std::printf(fails ? "hub_playtime_test: %d FAILED\n" : "hub_playtime_test: all ok\n", fails);
    return fails;
}
