#include "continue_logic.h"
#include "attract_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui::cont;
    const uint64_t now = 1800000000ull, H = 3600, D = 86400;

    /* ---- hero game ---- */
    {
        std::vector<Cand> g(4);
        g[0] = {"a", now - 5 * D, 100, false};
        g[1] = {"b", now - 2 * H, 7200, false};
        g[2] = {"c", now - 1 * H, 10, true};      // ghost: never
        g[3] = {"d", 0, 0, false};
        CHECK(pick_hero_game(g, now) == 1);
        g[1].last_played = now - 40 * D;          // outside the window
        CHECK(pick_hero_game(g, now) == 0);
        g[0].last_played = now - 31 * D;
        CHECK(pick_hero_game(g, now) == -1);      // new user / lapsed: today's hero
        CHECK(pick_hero_game({}, now) == -1);
        g[0].last_played = now - 30 * D;          // exactly on the edge counts
        CHECK(pick_hero_game(g, now) == 0);
        std::vector<Cand> t(2);
        t[0] = {"x", now - H, 0, false}; t[1] = {"y", now - H, 0, false};
        CHECK(pick_hero_game(t, now) == 0);       // tie keeps library order
        t[0].last_played = now + 50;              // slightly ahead clock
        CHECK(pick_hero_game(t, now) == 0);
    }
    /* ---- row ---- */
    {
        std::vector<Cand> g;
        for (int i = 0; i < 10; ++i) g.push_back({"g" + std::to_string(i), now - (uint64_t)(i + 1) * H, 0, false});
        g[2].ghost = true;
        g[5].last_played = 0;
        g[7].last_played = now - 90 * D;
        auto r = pick_row(g, now);
        CHECK(r.size() == 6);
        CHECK(r[0] == 0 && r[1] == 1 && r[2] == 3 && r[3] == 4 && r[4] == 6 && r[5] == 8);
        CHECK(pick_row(g, now, 4).size() == 4);
        CHECK(pick_row({}, now).empty());
        std::vector<Cand> one(1); one[0] = {"o", now - H, 0, false};
        CHECK(pick_row(one, now).size() == 1);
    }
    /* ---- backdrop ---- */
    {
        CHECK(pick_backdrop(true, true, true) == Backdrop::StateThumb);
        CHECK(pick_backdrop(false, true, true) == Backdrop::Screenshot);
        CHECK(pick_backdrop(false, false, true) == Backdrop::Cover);
        CHECK(pick_backdrop(false, false, false) == Backdrop::None);
        CHECK(pick_backdrop(true, false, false) == Backdrop::StateThumb);
        std::vector<Pic> p = {{"a", 10}, {"b", 30}, {"c", 20}, {"d", 30}};
        CHECK(newest_pic(p) == 1);
        CHECK(newest_pic({}) == -1);
    }
    /* ---- relative time ---- */
    {
        CHECK(relative_time(now, 0) == "never");
        CHECK(relative_time(now, now) == "just now");
        CHECK(relative_time(now, now - 59) == "just now");
        CHECK(relative_time(now, now + 10) == "just now");
        CHECK(relative_time(now, now - 60) == "1 minute ago");
        CHECK(relative_time(now, now - 5 * 60) == "5 minutes ago");
        CHECK(relative_time(now, now - 3599) == "59 minutes ago");
        CHECK(relative_time(now, now - H) == "1 hour ago");
        CHECK(relative_time(now, now - 2 * H - 5) == "2 hours ago");
        CHECK(relative_time(now, now - 23 * H) == "23 hours ago");
        CHECK(relative_time(now, now - D) == "yesterday");
        CHECK(relative_time(now, now - 3 * D) == "3 days ago");
        CHECK(relative_time(now, now - 14 * D) == "2 weeks ago");
        CHECK(relative_time(now, now - 45 * D) == "6 weeks ago");
        CHECK(relative_time(now, now - 61 * D) == "2 months ago");
        CHECK(total_time(30) == "");
        CHECK(total_time(40 * 60) == "40 minutes played");
        CHECK(total_time(2 * H + 1800) == "2.5 hours played");
        CHECK(total_time(14 * H + 100) == "14 hours played");
        CHECK(last_played_line(now, now - 2 * H, 14 * H, " | ") == "Last played 2 hours ago | 14 hours played");
        CHECK(last_played_line(now, now - 2 * H, 0, " | ") == "Last played 2 hours ago");
        CHECK(last_played_line(now, 0, 0, " | ") == "Not played yet");
    }
    /* ---- Continue vs Play ---- */
    {
        Action a = pick_action(true, true, false, false, false);
        CHECK(a.label == "Continue" && a.resume && a.enabled && a.show_fresh);
        a = pick_action(true, false, false, false, false);
        CHECK(a.label == "Play" && !a.resume && a.enabled && !a.show_fresh);   // no auto state
        a = pick_action(false, true, false, false, false);
        CHECK(a.label == "Play" && !a.resume);                                  // runner cannot load states (Dolphin, native...)
        a = pick_action(true, true, true, false, false);
        CHECK(a.label == "Playing now" && !a.enabled && !a.resume);
        a = pick_action(true, true, false, true, false);
        CHECK(a.label == "Continue" && !a.enabled);                             // another game holds the screen
        a = pick_action(true, true, false, false, true);
        CHECK(a.label == "Continue" && !a.enabled);                             // lock screen
    }
    /* ---- Ken Burns / fade ---- */
    {
        for (double t = 0; t < 120; t += 0.7) { float u = pingpong01(t); CHECK(u >= 0.f && u <= 1.f); }
        CHECK(pingpong01(0) < 0.001f && pingpong01(28) > 0.999f);
        namespace at = hubui::attract;
        for (int idx = 0; idx < 4; ++idx)
            for (float u = 0; u <= 1.f; u += 0.1f) {
                at::UV c = at::cover_uv(160, 100, 1200, 300), k = at::ken_burns(idx, u, c);
                CHECK(k.u0 >= c.u0 - 1e-4f && k.u1 <= c.u1 + 1e-4f && k.v0 >= c.v0 - 1e-4f && k.v1 <= c.v1 + 1e-4f);
            }
        CHECK(fade_step(0.f, 0.1f) > 0.f && fade_step(0.9f, 1.f) == 1.f && fade_step(0.f, 1.f, 0.f) == 1.f);
    }
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_continue_test: ok\n");
    return 0;
}
