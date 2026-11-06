// Window placement / UI scale logic (display_logic.h): monitor gone, partly off screen, maximized, DPI, min size.
#include "display_logic.h"
#include <cstdio>

using namespace hubdisplay;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static Monitor mon(const char* n, int x, int y, int w, int h, bool primary, int taskbar = 40) {
    Monitor m; m.name = n; m.bounds = Rect{x, y, w, h}; m.work = Rect{x, y, w, h - taskbar}; m.primary = primary; return m;
}

int main() {
    const std::vector<Monitor> two = {mon("\\.\DISPLAY1", 0, 0, 1920, 1080, true), mon("\\.\DISPLAY2", 1920, 0, 2560, 1440, false)};
    const std::vector<Monitor> one = {two[0]};
    Saved s;
    s.rect = Rect{2200, 100, 1280, 780};
    s.monitor = "\\.\DISPLAY2";

    { // normal restore on the second monitor
        Placement p = plan_start(s, true, two, 1280, 780);
        CHECK(p.restored && p.rect.x == 2200 && p.rect.y == 100 && p.rect.w == 1280 && p.monitor == s.monitor && !p.maximized);
    }
    { // remember off: centred on the chosen monitor (Open on), size = default
        Placement p = plan_start(s, false, two, 1280, 780);
        CHECK(!p.restored && p.monitor == s.monitor && p.rect.x == 1920 + (2560 - 1280) / 2);
    }
    { // monitor gone: centred on the primary, size kept
        Placement p = plan_start(s, true, one, 1280, 780);
        CHECK(!p.restored && p.monitor == one[0].name && p.rect.w == 1280 && p.rect.x == (1920 - 1280) / 2 && p.rect.x >= 0);
    }
    { // monitor gone and it was maximized: primary, still maximized
        Saved m = s; m.maximized = true;
        Placement p = plan_start(m, true, one, 1280, 780);
        CHECK(p.maximized && p.monitor == one[0].name);
    }
    { // partly off screen but still grabbable: kept
        Saved o = s; o.rect = Rect{4300, 100, 1280, 780};   // 140 px of a 1280 window on a monitor ending at 4480
        Placement p = plan_start(o, true, two, 1280, 780);
        CHECK(!p.restored && p.monitor == one[0].name);   // too little visible: primary
        o.rect = Rect{3800, 100, 1280, 780};              // 680 px visible
        p = plan_start(o, true, two, 1280, 780);
        CHECK(p.restored && p.rect.x == 3800);
    }
    { // title bar above the work area / below the bottom: not visible
        Saved o = s; o.rect = Rect{2200, -300, 1280, 780};
        CHECK(!plan_start(o, true, two, 1280, 780).restored);
        o.rect = Rect{2200, 1420, 1280, 780};
        CHECK(!plan_start(o, true, two, 1280, 780).restored);
    }
    { // resolution shrank: saved rect bigger than the monitor's work area is clamped
        Saved o = s; o.monitor = one[0].name; o.rect = Rect{0, 0, 3000, 2000};
        Placement p = plan_start(o, true, one, 1280, 780);
        CHECK(p.restored && p.rect.w == 1920 && p.rect.h == 1040);
    }
    { // maximized on the second monitor: monitor exists, so it opens maximized there even if the normal rect is stale
        Saved m = s; m.maximized = true; m.rect = Rect{-5000, -5000, 1280, 780};
        Placement p = plan_start(m, true, two, 1280, 780);
        CHECK(p.maximized && p.monitor == s.monitor && p.restored && rect_visible_on(p.rect, two[1]));
    }
    { // never saved: centred on the primary at the default size
        Placement p = plan_start(Saved{}, true, two, 1280, 780);
        CHECK(!p.restored && p.monitor == two[0].name && p.rect.w == 1280 && p.rect.h == 780);
    }
    { // empty monitor name means the primary
        Saved e; e.rect = Rect{100, 100, 1280, 780};
        Placement p = plan_start(e, true, two, 1280, 780);
        CHECK(!p.restored && p.monitor == two[0].name);   // no monitor recorded: treat as gone, centre on primary
        Placement q = plan_start(Saved{}, false, two, 1280, 780);
        CHECK(q.monitor == two[0].name);
    }
    { // no monitors at all
        Placement p = plan_start(s, true, {}, 1280, 780);
        CHECK(p.rect.w == 1280 && !p.restored);
    }
    { // Open on: move target
        Rect t = move_target("\\.\DISPLAY2", Rect{100, 100, 1280, 780}, two);
        CHECK(t.w == 1280 && t.x == 1920 + 640);
        CHECK(move_target("\\.\DISPLAY9", Rect{0, 0, 1280, 780}, two).w == 0);
        CHECK(move_target("", Rect{0, 0, 1280, 780}, two).x == 320);
        Rect big = move_target("\\.\DISPLAY1", Rect{0, 0, 5000, 5000}, two);
        CHECK(big.w == 1920 && big.h == 1040);
    }
    { // visibility helpers
        CHECK(visible_on_any(Rect{100, 100, 1280, 780}, two) && !visible_on_any(Rect{9000, 100, 1280, 780}, two));
        CHECK(monitor_of(Rect{2000, 0, 1280, 780}, two)->name == two[1].name && monitor_of(Rect{9000, 9000, 10, 10}, two) == nullptr);
    }
    { // minimum size: scaled, clamped to the work area (L3)
        Rect m = min_size_for(1.f, Rect{0, 0, 1920, 1040});
        CHECK(m.w == 960 && m.h == 600);
        m = min_size_for(2.f, Rect{0, 0, 1920, 1040});
        CHECK(m.w == 1920 && m.h == 1040);
        m = min_size_for(1.25f, Rect{0, 0, 1366, 728});
        CHECK(m.w == 1200 && m.h == 728);
    }
    { // UI scale
        CHECK(clamp_ui_scale(50) == 80 && clamp_ui_scale(500) == 200 && clamp_ui_scale(113) == 110 && clamp_ui_scale(115) == 120 && clamp_ui_scale(100) == 100);
        CHECK(std::fabs(effective_scale(1.5f, 150) - 2.25f) < 1e-4f && std::fabs(effective_scale(1.f, 80) - 0.8f) < 1e-4f);
    }
    { // reduce motion's first-run default
        CHECK(initial_reduce_motion(false, false, false) == true);    // never saved, Windows animations off
        CHECK(initial_reduce_motion(false, false, true) == false);    // never saved, animations on
        CHECK(initial_reduce_motion(true, false, false) == false);    // the player's own choice wins afterwards
        CHECK(initial_reduce_motion(true, true, true) == true);
    }
    std::printf(fails ? "display_test: %d FAILED\n" : "display_test: all passed\n", fails);
    return fails ? 1 : 0;
}
