#include "homescroll_logic.h"

#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui::hs;

    /* vertical wheel: always the page, over a row or not (this is the "caught on the rows" bug) */
    {
        WheelRouter w;
        Route r = w.route(0, -1, false, true, 1.0);   /* wheel down over a row */
        CHECK(r.page == 1.f && r.row == 0.f);
        r = w.route(0, 1, false, true, 2.0);          /* wheel up over a row */
        CHECK(r.page == -1.f && r.row == 0.f);
        r = w.route(0, -2, false, false, 3.0);
        CHECK(r.page == 2.f && r.row == 0.f);
    }
    /* horizontal wheel: only a row reacts */
    {
        WheelRouter w;
        Route r = w.route(1, 0, false, true, 1.0);
        CHECK(r.row == 1.f && r.page == 0.f);
        r = w.route(-1.5f, 0, false, true, 2.0);
        CHECK(r.row == -1.5f && r.page == 0.f);
        r = w.route(1, 0, false, false, 3.0);         /* off a row: nothing moves */
        CHECK(r.row == 0.f && r.page == 0.f);
    }
    /* Shift + wheel pans the row under the pointer (wheel up = left, down = right), the page elsewhere */
    {
        WheelRouter w;
        Route r = w.route(0, -1, true, true, 1.0);
        CHECK(r.row == 1.f && r.page == 0.f);
        r = w.route(0, 1, true, true, 2.0);
        CHECK(r.row == -1.f && r.page == 0.f);
        r = w.route(0, -1, true, false, 3.0);
        CHECK(r.page == 1.f && r.row == 0.f);
    }
    /* diagonal: the dominant axis wins and holds for the whole gesture */
    {
        WheelRouter w;
        Route r = w.route(0.2f, -1.f, false, true, 1.0);   /* mostly vertical */
        CHECK(r.page == 1.f && r.row == 0.f);
        r = w.route(0.9f, -0.3f, false, true, 1.05);       /* still the same gesture: stays vertical */
        CHECK(r.page == 0.3f && r.row == 0.f);
        r = w.route(0.9f, -0.3f, false, true, 2.0);        /* a new gesture: mostly horizontal now */
        CHECK(r.row == 0.9f && r.page == 0.f);
        r = w.route(0.1f, -0.8f, false, true, 2.05);       /* locked horizontal */
        CHECK(r.row == 0.1f && r.page == 0.f);
        r = w.route(0.f, -1.f, false, true, 2.06);         /* a pure vertical event always follows its axis */
        CHECK(r.page == 1.f && r.row == 0.f);
        r = w.route(0.f, 0.f, false, true, 2.07);
        CHECK(r.page == 0.f && r.row == 0.f);
    }
    /* spring: converges, never overshoots, frame-rate independent */
    {
        float p = 0, v = 0;
        float prev = 0;
        bool mono = true;
        for (int i = 0; i < 120; ++i) { spring_step(p, v, 400.f, 1.f / 60.f); if (p < prev - 1e-3f || p > 400.f + 0.5f) mono = false; prev = p; }
        CHECK(mono);
        CHECK(p == 400.f);
        float pa = 0, va = 0, pb = 0, vb = 0;
        for (int i = 0; i < 12; ++i) spring_step(pa, va, 300.f, 1.f / 60.f);    /* 0.2 s at 60 fps */
        for (int i = 0; i < 29; ++i) spring_step(pb, vb, 300.f, 1.f / 144.f);   /* ~0.2 s at 144 fps */
        CHECK(std::fabs(pa - pb) < 6.f);
        float p0 = 5, v0 = 0;
        spring_step(p0, v0, 50.f, 0.f);
        CHECK(p0 == 5.f);                  /* dt 0: untouched */
        /* momentum: moving and then retargeted back still glides on for a frame */
        float pm = 0, vm = 0;
        for (int i = 0; i < 6; ++i) spring_step(pm, vm, 500.f, 1.f / 60.f);
        float before = pm;
        spring_step(pm, vm, pm, 1.f / 60.f);
        CHECK(pm > before);
    }
    CHECK(notch_px(8.f) > 40.f);
    CHECK(clampf(-1, 0, 5) == 0 && clampf(9, 0, 5) == 5 && clampf(3, 0, 5) == 3);

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_homescroll_test OK\n");
    return 0;
}
