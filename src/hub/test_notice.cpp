// Console test: toasts and friend cards while a dialog is open (notice_logic.h): where a toast goes so it is clear of a real dialog, the
// fallback over it when nothing is clear, what counts as a real dialog (not a full-window page), and when a friend card hides and how its
// clock stands still meanwhile.
#include "notice_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::notice;

static Box B(float x0, float y0, float x1, float y1) { Box b; b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1; return b; }
static bool clear_of(const Box& t, const Box& d) { return !overlaps(t, d); }

/* A toast laid out at most max_w wide: 300 px of text plus 60 px of chrome on one line, wrapped to more lines when narrower (40 px a line). */
static Size measure_300(float max_w) {
    Size s;
    const float text = max_w - 60.f;
    if (text <= 0.f) return s;
    int lines = 1;
    while (300.f / lines > text) ++lines;
    s.w = 300.f / lines + 60.f;
    s.h = 40.f * lines + 20.f;
    s.fits = lines <= 6;
    return s;
}

static void real_dialog_rule() {
    CHECK(real_dialog(B(150, 50, 1130, 670), 1280, 720));     // a big dialog (77 % of the window)
    CHECK(!real_dialog(B(0, 0, 1280, 720), 1280, 720));      // the full-window game page: not a dialog
    CHECK(!real_dialog(B(10, 10, 1270, 710), 1280, 720));    // nearly full-window: not one either
    CHECK(real_dialog(B(32, 30, 992, 570), 1024, 600));      // the Cheats dialog at 1024x600, UI size 150 % (84 %)
}

static void toast_places() {
    const Box bounds = B(12, 12, 1256, 708);
    const Box def = B(856, 580, 1256, 640);   // 400 x 60, bottom right, its usual place
    std::vector<Box> none;
    {   // no dialog, or a dialog it does not touch: its usual place, exactly
        Placed p = place_toast(def, bounds, nullptr, none, 8.f, false, measure_300, 160.f);
        CHECK(p.spot == Spot::Default && p.box.x0 == def.x0 && p.box.y0 == def.y0 && p.box.x1 == def.x1 && p.box.y1 == def.y1);
        const Box small = B(440, 260, 840, 460);
        p = place_toast(def, bounds, &small, none, 8.f, false, measure_300, 160.f);
        CHECK(p.spot == Spot::Default);
    }
    {   // a dialog over its usual place with room under it: below the dialog, right-aligned, clear of it, inside the window
        const Box d = B(300, 100, 980, 600);
        Placed p = place_toast(def, bounds, &d, none, 8.f, false, measure_300, 160.f);
        CHECK(p.spot == Spot::Below && clear_of(p.box, d) && inside(p.box, bounds) && p.box.x1 == def.x1 && p.box.y0 >= d.y1 && p.box.y1 == bounds.y1);
        // a second toast stacks above the first in the same spot while it fits there, then moves on
        std::vector<Box> taken = {p.box};
        Placed q = place_toast(def, bounds, &d, taken, 8.f, false, measure_300, 160.f);
        CHECK(q.spot != Spot::Over && clear_of(q.box, d) && !overlaps(q.box, p.box) && inside(q.box, bounds));
    }
    {   // a tall dialog that leaves a band above it only: above the dialog
        const Box d = B(300, 200, 980, 700);
        Placed p = place_toast(def, bounds, &d, none, 8.f, false, measure_300, 160.f);
        CHECK(p.spot == Spot::Above && clear_of(p.box, d) && inside(p.box, bounds) && p.box.y1 <= d.y0);
    }
    {   // a full-height dialog with room at its right: beside it, re-wrapped narrower and never cut
        const Box d = B(250, 20, 1000, 700);
        Placed p = place_toast(def, bounds, &d, none, 8.f, false, measure_300, 160.f);
        CHECK(p.spot == Spot::Right && clear_of(p.box, d) && inside(p.box, bounds) && p.box.w() <= bounds.x1 - d.x1 && p.box.h() > def.h());
    }
    {   // room on the left only
        const Box d = B(400, 20, 1250, 700);
        Placed p = place_toast(def, bounds, &d, none, 8.f, false, measure_300, 160.f);
        CHECK(p.spot == Spot::Left && clear_of(p.box, d) && inside(p.box, bounds) && p.box.x0 == bounds.x0);
    }
    {   // the side is too narrow for the text without cutting it: no side spot
        const Box d = B(100, 20, 1150, 700);   // 106 px right, 88 px left: the text would need 7 and 11 lines
        Placed p = place_toast(def, bounds, &d, none, 8.f, false, measure_300, 80.f);
        CHECK(p.spot == Spot::Over);
    }
    {   // nothing clear (the Cheats dialog at 1024x600, 150 %): drawn where it always was, over the dialog, never held back
        const Box b2 = B(12, 12, 999, 588), d = B(20, 18, 1004, 582), def2 = B(599, 470, 999, 530);
        Placed p = place_toast(def2, b2, &d, none, 8.f, false, measure_300, 160.f);
        CHECK(p.spot == Spot::Over && p.box.x0 == def2.x0 && p.box.y0 == def2.y0 && p.box.x1 == def2.x1 && p.box.y1 == def2.y1);
    }
    {   // a top toast (stacks from the top edge) tries above the dialog first
        const Box tdef = B(856, 60, 1256, 120), d = B(300, 40, 980, 600);
        Placed p = place_toast(tdef, bounds, &d, none, 8.f, true, measure_300, 160.f);
        CHECK(p.spot == Spot::Below && clear_of(p.box, d) && p.box.y0 >= d.y1);   // no room above (40 px), so below
        const Box d2 = B(300, 100, 980, 690);
        p = place_toast(tdef, bounds, &d2, none, 8.f, true, measure_300, 160.f);
        CHECK(p.spot == Spot::Above && clear_of(p.box, d2) && p.box.y0 == bounds.y0);
    }
    {   // no measure given: a side spot is used only when the toast already fits there as it is
        const Box d = B(250, 20, 800, 700);   // 456 px free at the right: the 400 px toast fits unchanged
        Placed p = place_toast(def, bounds, &d, none, 8.f, false, nullptr, 160.f);
        CHECK(p.spot == Spot::Default);   // (its usual place does not touch this dialog at all)
        const Box d2 = B(250, 20, 1000, 700);
        p = place_toast(def, bounds, &d2, none, 8.f, false, nullptr, 160.f);
        CHECK(p.spot == Spot::Over);
    }
}

static void cards() {
    const Box dlg = B(300, 100, 980, 600);
    const std::vector<Box> popups = {dlg};
    // hidden only while it would lie over a dialog or menu, or past the top edge
    CHECK(card_hidden(B(900, 450, 1260, 560), popups, 8.f));
    CHECK(!card_hidden(B(990, 450, 1260, 560), popups, 8.f));   // beside the dialog: shown
    CHECK(!card_hidden(B(900, 450, 1260, 560), {}, 8.f));       // no dialog (a full-window page is never in the list): shown
    CHECK(card_hidden(B(900, 4, 1260, 120), {}, 8.f));          // its top past the window's top edge: hidden, not cut
    // the clock stands still while hidden: an invite card (30 s) hidden for 60 s still has its whole time afterwards
    const double life = 30.0, dt = 0.5;
    double t0 = 0.0, now = 0.0;
    for (int i = 0; i < 120; ++i) {   // 60 s hidden behind a dialog
        now += dt;
        CardTick k = card_tick(true, t0, now, dt, life);
        CHECK(!k.draw && !k.expired);
        t0 = k.t0;
    }
    CHECK(now - t0 < 1e-9);   // age still 0
    int shown = 0;
    bool expired = false;
    for (int i = 0; i < 100 && !expired; ++i) {
        now += dt;
        CardTick k = card_tick(false, t0, now, dt, life);
        t0 = k.t0;
        if (k.draw) ++shown;
        expired = k.expired;
    }
    CHECK(shown == 60 && expired);   // 30 s on screen, then gone
    CardTick k = card_tick(true, 10.0, 50.0, -1.0, life);   // a clock oddity never ages a hidden card
    CHECK(k.t0 == 10.0 && !k.draw && !k.expired);
}

int main() {
    real_dialog_rule();
    toast_places();
    cards();
    std::printf(fails ? "hub_notice_test FAILED\n" : "hub_notice_test OK\n");
    return fails ? 1 : 0;
}
