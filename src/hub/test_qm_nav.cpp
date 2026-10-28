// Console test for the quick menu's own controls (qm_nav.h): release gate, edges, repeat, row selection and actions.
#include "qm_nav.h"
#include <cstdio>
using namespace hubqm;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } else ++g_pass; } while (0)

static void draw_main(MenuNav& n, NavAction* got, const char* want_id) {
    const char* ids[] = {"resume", "speed", "volume", "mute", "save", "quit"};
    bool step[] = {false, true, true, true, false, false};
    for (int i = 0; i < 6; ++i) { NavAction a = n.add(ids[i], step[i], true, 0, i); if (!a.id.empty() && got) *got = a; }
    (void)want_id;
    n.end();
}

int main() {
    {   // release gate: everything held at open (View+Y, A and B from the game) does nothing until released
        MenuInput in; in.open(NB_A | NB_B);
        NavEvents e = in.update(NB_A | NB_B, 0);
        CHECK(!e.any());
        e = in.update(NB_A | NB_B, 500);           // still held: still nothing, no repeat
        CHECK(!e.any());
        e = in.update(NB_B, 520);                  // A released
        CHECK(!e.any());
        e = in.update(NB_A | NB_B, 540);           // A pressed again: real press; B still the old hold
        CHECK(e.activate && !e.back);
        e = in.update(0, 560); CHECK(!e.any());
        e = in.update(NB_B, 580); CHECK(e.back);   // a fresh B
    }
    {   // a held direction at open does not move, nor repeat
        MenuInput in; in.open(NB_DOWN);
        for (int t = 0; t < 2000; t += 16) CHECK(!in.update(NB_DOWN, t).any());
        CHECK(!in.update(0, 2016).any());
        CHECK(in.update(NB_DOWN, 2032).dy == 1);
    }
    {   // repeat: one step at press, then after 380 ms every 110 ms
        MenuInput in; in.open(0);
        int steps = 0;
        for (int t = 0; t <= 1000; t += 10) steps += in.update(NB_RIGHT, t).dx;
        CHECK(steps == 1 + 1 + (1000 - 380) / 110);   // t=0, t=380, then +110 ...
        CHECK(in.update(0, 1010).dx == 0);
    }
    {   // a press shorter than a frame (down + up between two polls) is not lost
        MenuInput in2; in2.open(0);
        CHECK(in2.update(0, 16, NB_DOWN).dy == 1);
        CHECK(in2.update(0, 32).dy == 0);
    }
    {   // a duplicate / late down EVENT for a button that is still held is not a press
        MenuInput in; in.open(NB_A);
        CHECK(!in.update(NB_A, 16, NB_A).any());       // held since the open: gate holds even with a duplicate event
        MenuInput in3; in3.open(0);
        CHECK(in3.update(NB_A, 16, NB_A).activate);    // a real press: one activation...
        CHECK(!in3.update(NB_A, 32, NB_A).any());      // ...not two from a duplicate event while held
        CHECK(!in3.update(0, 48, NB_A).any());         // nor a stray event at the release
    }
    {   // back wins over activate in the same frame
        MenuInput in; in.open(0);
        NavEvents e = in.update(NB_A | NB_B, 0);
        CHECK(e.back && !e.activate);
    }
    {   // nav: first enabled row selected, moves, clamps, steppers step, A activates
        MenuNav n; n.reset();
        n.begin(NavEvents()); draw_main(n, nullptr, nullptr);
        CHECK(n.sel == "resume");
        NavEvents up; up.dy = -1;
        n.begin(up); draw_main(n, nullptr, nullptr); CHECK(n.sel == "resume");   // clamp at top
        NavEvents dn; dn.dy = 1;
        n.begin(dn); draw_main(n, nullptr, nullptr); CHECK(n.sel == "speed");
        NavEvents rt; rt.dx = 1; NavAction a;
        n.begin(rt); draw_main(n, &a, nullptr); CHECK(a.id == "speed" && a.step == 1 && !a.activate && n.sel == "speed");
        NavEvents lf; lf.dx = -1; a = NavAction();
        n.begin(lf); draw_main(n, &a, nullptr); CHECK(a.id == "speed" && a.step == -1);
        NavEvents ac; ac.activate = true; a = NavAction();
        n.begin(ac); draw_main(n, &a, nullptr); CHECK(a.id == "speed" && a.step == 1);   // A on a stepper = +1
        for (int i = 0; i < 10; ++i) { n.begin(dn); draw_main(n, nullptr, nullptr); }
        CHECK(n.sel == "quit");   // clamp at bottom
        a = NavAction();
        n.begin(ac); draw_main(n, &a, nullptr); CHECK(a.id == "quit" && a.activate);
        NavEvents bk; bk.back = true;
        n.begin(bk); CHECK(n.back); draw_main(n, nullptr, nullptr);
    }
    {   // disabled rows are skipped; a selected row that disappears falls back to the first enabled row
        MenuNav n; n.reset();
        auto draw = [&](bool speed_on, bool with_save) {
            n.add("resume", false, true, 0, 0);
            n.add("speed", true, speed_on, 0, 1);
            n.add("volume", true, true, 0, 2);
            if (with_save) n.add("save", false, true, 0, 3);
            n.end();
        };
        n.begin(NavEvents()); draw(false, true);
        NavEvents dn; dn.dy = 1;
        n.begin(dn); draw(false, true); CHECK(n.sel == "volume");
        n.begin(dn); draw(false, true); CHECK(n.sel == "save");
        n.begin(NavEvents()); draw(false, false); CHECK(n.sel == "resume");
        NavEvents rt; rt.dx = 1;   // disabled stepper never acts
        n.sel = "speed"; NavAction a;
        n.begin(rt); a = n.add("resume", false, true, 0, 0); CHECK(a.id.empty()); a = n.add("speed", true, false, 0, 1); CHECK(a.id.empty()); n.end();
    }
    {   // grid (slot pages): 5 columns + Back below
        MenuNav n; n.reset();
        auto draw = [&]() { for (int i = 0; i < 10; ++i) n.add("slot" + std::to_string(i), false, true, i % 5, i / 5); n.add("back", false, true, 0, 2); n.end(); };
        n.begin(NavEvents()); draw(); CHECK(n.sel == "slot0");
        NavEvents rt, dn; rt.dx = 1; dn.dy = 1;
        n.begin(rt); draw(); n.begin(rt); draw(); CHECK(n.sel == "slot2");
        n.begin(dn); draw(); CHECK(n.sel == "slot7");
        n.begin(dn); draw(); CHECK(n.sel == "back");
        NavEvents up; up.dy = -1;
        n.begin(up); draw(); CHECK(n.sel == "slot5");
        for (int i = 0; i < 8; ++i) { n.begin(rt); draw(); }
        CHECK(n.sel == "slot9");
    }
    std::printf("qm_nav: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
