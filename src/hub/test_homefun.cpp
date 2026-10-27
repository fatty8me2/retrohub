// Console test for Wave 3 L6/L7/L8: attract-mode idle + slideshow maths, Surprise-me pick weighting and reel, Wrapped aggregation.
#include "attract_logic.h"
#include "surprise_logic.h"
#include "wrapped_logic.h"

#include <cstdio>
#include <map>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui;

static void test_attract() {
    attract::Idle id;
    bool woke = false;
    for (int i = 0; i < 59; ++i) woke |= id.step(1.0, false, true);
    CHECK(!id.active && !woke);                       // 59 s: not yet
    id.step(1.0, false, true);
    CHECK(id.active);                                 // 60 s: starts
    for (int i = 0; i < 5; ++i) id.step(1.0, false, true);
    CHECK(id.active && id.shown >= 4.9 && id.shown <= 5.1);
    CHECK(id.step(0.016, true, true));                // any input ends it and reports the wake
    CHECK(!id.active && id.idle == 0 && id.just_woke);   // that frame is flagged so its press can be swallowed
    id.step(0.016, false, true);
    CHECK(!id.just_woke);                              // and only that frame
    CHECK(!id.step(0.016, true, true));               // input while idle-inactive: no "wake"
    for (int i = 0; i < 100; ++i) id.step(1.0, false, false);
    CHECK(!id.active);                                // not eligible (other page, no focus, game running): never starts
    id.step(30, false, true);
    id.step(1.0, false, false);                       // losing eligibility resets the timer
    CHECK(id.idle == 0);
    attract::Idle fast;
    fast.step(2.0, false, true, 1.5);
    CHECK(fast.active);                               // custom threshold (screenshot aid)

    CHECK(attract::slide_at(0, 0).cur == -1);
    auto f0 = attract::slide_at(0.0, 3);
    CHECK(f0.cur == 0 && f0.next == 1 && f0.mix == 0.f);
    auto f1 = attract::slide_at(attract::kSlideSecs - attract::kFadeSecs / 2, 3);
    CHECK(f1.cur == 0 && f1.next == 1 && f1.mix > 0.45f && f1.mix < 0.55f);
    auto f2 = attract::slide_at(attract::kSlideSecs * 3 + 1.0, 3);
    CHECK(f2.cur == 0);                               // wraps around
    CHECK(attract::slide_at(attract::kSlideSecs - 0.01, 1).mix == 0.f);   // one picture: nothing to fade to

    attract::UV c = attract::cover_uv(160, 100, 400, 100);   // box much wider than the picture: crop top and bottom
    CHECK(c.u0 == 0.f && c.u1 == 1.f && c.v0 > 0.29f && c.v0 < 0.31f && c.v1 > 0.69f && c.v1 < 0.71f);
    attract::UV c2 = attract::cover_uv(100, 100, 100, 100);
    CHECK(c2.u0 == 0.f && c2.v1 == 1.f);
    bool inside = true, zooms = true;
    for (int idx = 0; idx < 4; ++idx)
        for (int k = 0; k <= 20; ++k) {
            attract::UV u = attract::ken_burns(idx, k / 20.f, c);
            inside &= u.u0 >= c.u0 - 1e-5f && u.u1 <= c.u1 + 1e-5f && u.v0 >= c.v0 - 1e-5f && u.v1 <= c.v1 + 1e-5f;
        }
    CHECK(inside);
    attract::UV a = attract::ken_burns(0, 0.f, c), b = attract::ken_burns(0, 1.f, c);
    zooms = (b.u1 - b.u0) < (a.u1 - a.u0);            // even pictures zoom in
    CHECK(zooms);
    attract::UV a1 = attract::ken_burns(1, 0.f, c), b1 = attract::ken_burns(1, 1.f, c);
    CHECK((b1.u1 - b1.u0) > (a1.u1 - a1.u0));        // odd pictures zoom out

    std::vector<std::vector<attract::Slide>> pg(3);
    pg[0] = {{"a", "A", "a1", 0, false}, {"a", "A", "a2", 0, false}, {"a", "A", "a3", 0, false}};
    pg[1] = {{"b", "B", "b1", 0, false}};
    pg[2] = {{"c", "C", "c1", 0, false}, {"c", "C", "c2", 0, false}};
    auto il = attract::interleave(pg);
    CHECK(il.size() == 6 && il[0].path == "a1" && il[1].path == "b1" && il[2].path == "c1" && il[3].path == "a2" && il[4].path == "c2" && il[5].path == "a3");
    CHECK(attract::interleave({}).empty());
}

static void test_surprise() {
    using namespace surprise;
    const int64_t now = 1000LL * 86400, D = 86400;
    std::vector<Cand> cs = {
        {"never", 0, false},
        {"recent", now - 3 * D, true},          // played 3 days ago: never picked
        {"old", now - 20 * D, false},
        {"oldfav", now - 20 * D, true},
        {"never_fav", 0, true},
    };
    CHECK(weight(cs[1], now) == 0.0);
    CHECK(quiet_enough(cs[0], now) && !quiet_enough(cs[1], now) && quiet_enough(cs[2], now));
    CHECK(weight(cs[0], now) > weight(cs[2], now));                 // never played beats played-a-while-ago
    CHECK(weight(cs[3], now) > weight(cs[2], now) * 2.0);           // favorite counts more
    CHECK(weight(cs[4], now) > weight(cs[0], now));
    Cand ancient{"ancient", now - 400 * D, false};
    CHECK(weight(ancient, now) > weight(cs[2], now));               // waiting longer helps a little...
    CHECK(weight(ancient, now) < weight(cs[0], now));               // ...but not past a game never played
    Cand edge{"edge", now - 14 * D, false};
    CHECK(weight(edge, now) > 0.0);                                 // exactly 14 days counts as 14+

    // distribution: sweep u across [0,1) and count how often each candidate is picked
    std::map<std::string, int> hits;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        Pick p = pick(cs, now, (i + 0.5) / N);
        CHECK(p.index >= 0 && !p.all_recent);
        hits[cs[p.index].key]++;
    }
    CHECK(hits["recent"] == 0);
    double total = 0;
    for (int i = 0; i < (int)cs.size(); ++i) total += weight(cs[i], now);
    for (int i = 0; i < (int)cs.size(); ++i) {
        const double want = weight(cs[i], now) / total * N;
        CHECK(hits[cs[i].key] > want - 5 && hits[cs[i].key] < want + 5);
    }
    CHECK(hits["never_fav"] > hits["never"] && hits["never"] > hits["old"]);

    // never repeats the last pick when something else is possible
    for (int i = 0; i < 200; ++i) {
        Pick p = pick(cs, now, (i + 0.5) / 200, "never_fav");
        CHECK(p.index >= 0 && cs[p.index].key != "never_fav");
    }
    // the only quiet game is the one just suggested: it is still offered
    std::vector<Cand> one = {{"solo", 0, false}, {"busy", now - D, false}};
    Pick ps = pick(one, now, 0.5, "solo");
    CHECK(ps.index == 0 && !ps.all_recent);
    // everything played lately: falls back to the whole library and says so
    std::vector<Cand> busy = {{"x", now - D, false}, {"y", now - 2 * D, true}};
    Pick pb = pick(busy, now, 0.99);
    CHECK(pb.index >= 0 && pb.all_recent);
    Pick pb2 = pick(busy, now, 0.1, "x");
    CHECK(pb2.index == 1 && pb2.all_recent);
    CHECK(pick({}, now, 0.5).index == -1);
    CHECK(pick(one, now, 5.0).index >= 0 && pick(one, now, -1.0).index >= 0);   // u out of range is clamped

    // reel
    std::vector<int> r = make_reel(5, 3, 20, 77);
    CHECK(r.size() == 20 && r.back() == 3);
    bool norep = true;
    for (size_t i = 1; i < r.size(); ++i) norep &= r[i] != r[i - 1];
    CHECK(norep);
    CHECK(make_reel(5, 3, 20, 77) == r);                             // repeatable
    CHECK(make_reel(5, 3, 20, 78) != r);
    CHECK(make_reel(1, 0, 6, 1).size() == 6 && make_reel(0, 0, 6, 1).empty());
    CHECK(spin_pos(0, 2.4, 20) == 0.f);
    CHECK(spin_pos(2.4, 2.4, 20) == 19.f && spin_pos(10, 2.4, 20) == 19.f);
    bool mono = true;
    float prev = -1, prev_step = 1e9f;
    for (int i = 0; i <= 48; ++i) {
        float p = spin_pos(i * 0.05, 2.4, 20);
        mono &= p >= prev;
        if (i > 0) { mono &= (p - prev) <= prev_step + 1e-4f; prev_step = p - prev; }   // each step is no bigger than the one before: it slows down
        prev = p;
    }
    CHECK(mono);
}

static hub::saves::Session S(const char* sys, const char* key, int64_t start, int64_t secs) {
    hub::saves::Session s;
    s.system = sys; s.key = key; s.start = start; s.secs = secs;
    return s;
}

static void test_wrapped() {
    using namespace wrapped;
    CHECK(days_from_civil(1970, 1, 1) == 0 && days_from_civil(2000, 3, 1) == 11017);
    int y, m, dy;
    civil_from_days(days_from_civil(2026, 10, 27), &y, &m, &dy);
    CHECK(y == 2026 && m == 10 && dy == 27);
    civil_from_days(days_from_civil(2024, 2, 29), &y, &m, &dy);
    CHECK(y == 2024 && m == 2 && dy == 29);
    CHECK(weekday_of_day(0) == 4 && weekday_of_day(days_from_civil(2026, 10, 27)) == 2 && weekday_of_day(-1) == 3);

    const int tz = 0;
    const int64_t day = 86400;
    const int64_t oct1 = days_from_civil(2026, 10, 1) * day;
    const int64_t now = oct1 + 26 * day + 12 * 3600;   // 2026-10-27 12:00
    Range mo = period_range(Period::Month, now, tz);
    CHECK(mo.from == oct1 && mo.to == days_from_civil(2026, 11, 1) * day && mo.year == 2026 && mo.month == 10);
    Range prev = period_range(Period::Month, now, tz, 1);
    CHECK(prev.month == 9 && prev.to == oct1);
    Range jan = period_range(Period::Month, days_from_civil(2026, 1, 15) * day, tz, 1);
    CHECK(jan.year == 2025 && jan.month == 12 && jan.to == days_from_civil(2026, 1, 1) * day);
    Range yr = period_range(Period::Year, now, tz);
    CHECK(yr.from == days_from_civil(2026, 1, 1) * day && yr.to == days_from_civil(2027, 1, 1) * day && yr.month == 0);
    CHECK(period_range(Period::Year, now, tz, 1).year == 2025);
    CHECK(period_label(mo) == "October 2026" && period_label(yr) == "2026");
    // a time zone shifts the boundary: 23:30 UTC on 30 Sep is already 1 Oct in UTC+1
    Range tzr = period_range(Period::Month, oct1 + 3600, 3600);
    CHECK(tzr.month == 10 && tzr.from == oct1 - 3600);
    Range tzr2 = period_range(Period::Month, oct1 - 1800, 3600);   // 23:30 UTC on 30 Sep = 00:30 on 1 Oct local
    CHECK(tzr2.month == 10);

    std::vector<hub::saves::Session> ss = {
        S("n64", "mk", oct1 + 1 * day + 3600, 3600),               // Oct 2 (Fri)
        S("n64", "mk", oct1 + 2 * day + 3600, 7200),               // Oct 3
        S("n64", "mk", oct1 + 3 * day + 3600, 1800),               // Oct 4
        S("gba", "mf", oct1 + 3 * day + 9000, 900),                // Oct 4 again (same day)
        S("gba", "mf", oct1 + 10 * day, 5400),                     // Oct 11 (longest session)
        S("n64", "sm", oct1 + 11 * day, 600),                      // Oct 12
        S("n64", "mk", oct1 - 5 * day, 99999),                     // September: outside
        S("n64", "mk", oct1 + 40 * day, 99999),                    // November: outside
        S("gb", "bad", oct1 + 5 * day, 0),                         // zero length: ignored
    };
    std::vector<GroupEvent> gr = {
        {oct1 + 4 * day, {{"u1", "Mia"}, {"u2", "Kai"}}},
        {oct1 + 12 * day, {{"u1", "Mia (new)"}}},
        {oct1 + 13 * day, {{"u2", "Kai"}, {"u3", "Jo"}}},
        {oct1 - 2 * day, {{"u3", "Jo"}, {"u3", "Jo"}}},            // outside the month
    };
    Data d = aggregate(ss, gr, Period::Month, mo, tz);
    CHECK(!d.empty() && d.sessions == 6);
    CHECK(d.total_secs == 3600 + 7200 + 1800 + 900 + 5400 + 600);
    CHECK(d.active_days == 5 && d.games_played == 3);
    CHECK(d.longest_secs == 7200 && d.longest_start == oct1 + 2 * day + 3600);
    CHECK(d.longest_streak == 3);                                  // Oct 2, 3, 4
    CHECK(d.top.size() == 3 && d.top[0].key == "mk" && d.top[0].secs == 12600 && d.top[0].sessions == 3);
    CHECK(d.top[1].key == "mf" && d.top[1].secs == 6300 && d.top[2].key == "sm");
    CHECK(d.systems.size() == 2 && d.systems[0].first == "n64" && d.systems[0].second == 13200 && d.systems[1].first == "gba" && d.systems[1].second == 6300);
    CHECK(d.has_friend && d.best_friend.uid == "u2" && d.best_friend.together == 2 && d.best_friend.name == "Kai");   // u1 and u2 tie at 2: u2 played with you most recently
    CHECK(d.busiest_weekday >= 0 && d.busiest_weekday_secs > 0);

    Data empty = aggregate(ss, gr, Period::Month, period_range(Period::Month, now, tz, 3), tz);
    CHECK(empty.empty() && empty.top.empty() && !empty.has_friend && empty.longest_streak == 0 && empty.busiest_weekday == -1);

    Data year = aggregate(ss, gr, Period::Year, yr, tz, 2);
    CHECK(year.top.size() == 2 && year.sessions == 8);             // the year includes September and November
    CHECK(year.top[0].key == "mk" && year.top[0].secs == 12600 + 2 * 99999);
    CHECK(year.has_friend && year.best_friend.uid == "u2" && year.best_friend.together == 2);   // u1, u2, u3 all twice (u3's duplicate in one group counts once); u2 and u3 tie on recency, the first wins

    // a gap breaks the streak, and the longest run wins
    std::vector<hub::saves::Session> ss2;
    for (int dn : {1, 2, 4, 5, 6, 7, 20}) ss2.push_back(S("n64", "g", oct1 + dn * day + 100, 60));
    CHECK(aggregate(ss2, {}, Period::Month, mo, tz).longest_streak == 4);
    CHECK(!aggregate(ss2, {}, Period::Month, mo, tz).has_friend);

    CHECK(hours_text(0) == "0 min" && hours_text(30) == "<1 min" && hours_text(5 * 60) == "5 min");
    CHECK(hours_text(3600) == "1 h" && hours_text(3600 + 30 * 60) == "1 h 30 min");
    CHECK(std::string(weekday_name(0)) == "Sunday" && std::string(weekday_name(9)).empty());
}

int main() {
    test_attract();
    test_surprise();
    test_wrapped();
    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
