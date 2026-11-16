/* hub_uikit_test: the UI kit's layout math (ui_layout_logic.h). */
#include "ui_layout_logic.h"

#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

int main() {
    using namespace hubui::kit;

    /* ---- paging ---- */
    CHECK(near(page_step(400.f), 340.f));
    CHECK(page_step(0.f) >= 1.f);
    CHECK(near(page_target(0.f, 1000.f, 400.f, +1), 340.f));
    CHECK(near(page_target(900.f, 1000.f, 400.f, +1), 1000.f));     // clamps at the end
    CHECK(near(page_target(100.f, 1000.f, 400.f, -1), 0.f));        // and at the top
    CHECK(near(page_target(500.f, 1000.f, 400.f, 0), 500.f));
    CHECK(near(page_target(50.f, -5.f, 400.f, +1), 0.f));           // nothing to scroll
    CHECK(can_page(0.f, 10.f, +1) && !can_page(0.f, 10.f, -1));
    CHECK(!can_page(10.f, 10.f, +1) && can_page(10.f, 10.f, -1));
    CHECK(!can_page(0.f, 0.f, +1) && !can_page(0.f, 0.f, -1));
    CHECK(!overflows(0.f) && !overflows(0.4f) && overflows(1.f));

    /* ---- focus follow ---- */
    {
        const float unit = 8.f;
        CHECK(near(focus_margin(600.f, unit), 64.f));                  // about one row
        CHECK(near(focus_margin(100.f, unit), 25.f));                  // never more than a quarter of the view
        CHECK(near(focus_margin(-5.f, unit), 0.f));
        /* view 100..500 (400 tall), margin 64 */
        CHECK(near(follow_scroll(200.f, 2000.f, 100.f, 500.f, 200.f, 240.f, 64.f), 200.f));   // comfortably inside: unchanged
        CHECK(near(follow_scroll(200.f, 2000.f, 100.f, 500.f, 470.f, 510.f, 64.f), 200.f + 74.f));   // below: bottom + margin
        CHECK(near(follow_scroll(200.f, 2000.f, 100.f, 500.f, 120.f, 160.f, 64.f), 200.f - 44.f));   // above: top + margin
        CHECK(near(follow_scroll(20.f, 2000.f, 100.f, 500.f, 40.f, 80.f, 64.f), 0.f));               // clamps at the top
        CHECK(near(follow_scroll(1980.f, 2000.f, 100.f, 500.f, 600.f, 640.f, 64.f), 2000.f));        // and at the end
        CHECK(near(follow_scroll(0.f, 2000.f, 100.f, 500.f, 600.f, 1200.f, 64.f), 500.f));           // taller than the view: top aligned
        CHECK(near(follow_scroll(700.f, 2000.f, 100.f, 500.f, 50.f, 900.f, 64.f), 700.f));           // already covering the view: left alone
        CHECK(near(follow_scroll(0.f, 2000.f, 100.f, 500.f, 470.f, 840.f, 64.f), 355.f));            // nearly view-tall item: the margin shrinks to what is left (15)
        CHECK(near(follow_scroll(200.f, 2000.f, 100.f, 500.f, 480.f, 520.f, 0.f), 220.f));           // margin 0: just fully visible
    }

    /* ---- scrollbar thumb mirrors ImGui::ScrollbarEx ---- */
    {
        Track t = scrollbar_track(0.f, 0.f, 12.f, 400.f);
        CHECK(near(t.x0, 3.f) && near(t.x1, 9.f) && near(t.y0, 3.f) && near(t.y1, 397.f));
        Track thin = scrollbar_track(0.f, 0.f, 4.f, 400.f);
        CHECK(near(thin.x0, 1.f) && near(thin.x1, 3.f));
        Thumb none = scrollbar_thumb(0.f, 394.f, 400.f, 400.f, 0.f, 10.f);
        CHECK(!none.show);                                             // fits: no thumb
        Thumb a = scrollbar_thumb(0.f, 400.f, 400.f, 800.f, 0.f, 10.f);
        CHECK(a.show && near(a.y0, 0.f) && near(a.y1, 200.f));          // half visible: half-height thumb at the top
        Thumb b = scrollbar_thumb(0.f, 400.f, 400.f, 800.f, 400.f, 10.f);
        CHECK(near(b.y0, 200.f) && near(b.y1, 400.f));                  // at the end
        Thumb c = scrollbar_thumb(0.f, 400.f, 400.f, 400000.f, 0.f, 12.f);
        CHECK(near(c.y1 - c.y0, 12.f));                                 // never smaller than the grab minimum
        Thumb d = scrollbar_thumb(0.f, 400.f, 400.f, 800.f, 9999.f, 10.f);
        CHECK(near(d.y1, 400.f));                                       // over-scrolled: clamped
    }

    /* ---- sheet split: the body takes all remaining height ---- */
    {
        Split s = sheet_split(0.f, 700.f, 120.f, 60.f, 10.f);
        CHECK(near(s.body_y, 130.f) && near(s.footer_y, 640.f) && near(s.body_h, 500.f));
        Split nofoot = sheet_split(0.f, 700.f, 120.f, 0.f, 10.f);
        CHECK(near(nofoot.body_h, 570.f) && near(nofoot.footer_y, 700.f));
        Split nohead = sheet_split(20.f, 700.f, 0.f, 60.f, 10.f);
        CHECK(near(nohead.body_y, 20.f) && near(nohead.body_h, 610.f));
        Split tiny = sheet_split(0.f, 100.f, 80.f, 60.f, 10.f);
        CHECK(near(tiny.body_h, 0.f) && tiny.footer_y >= tiny.body_y);   // too short: body 0, footer never above the body
        for (float h = 0.f; h < 2000.f; h += 37.f) {
            Split q = sheet_split(0.f, h, 100.f, 50.f, 8.f);
            CHECK(q.body_h >= 0.f);
            if (h >= 166.f) CHECK(near(q.body_y + q.body_h + 8.f + 50.f, h));   // header + gap + body + gap + footer == height
        }
    }

    /* ---- dialog sizes ---- */
    {
        CHECK(near(dialog_width(DlgSmall, 8.f, 1920.f), 416.f));
        CHECK(near(dialog_width(DlgMedium, 8.f, 1920.f), 528.f));
        CHECK(near(dialog_width(DlgLarge, 8.f, 1920.f), 704.f));
        CHECK(near(dialog_width(DlgLarge, 12.f, 800.f), 736.f));          // capped at 92 % of the window
        CHECK(near(dialog_width(7, 8.f, 1920.f), 704.f));                 // out of range -> nearest class
        CHECK(near(dialog_max_h(720.f), 648.f));
        CHECK(near(dialog_body_max(648.f, 24.f, 40.f, 50.f, 20.f, 60.f), 490.f));
        CHECK(near(dialog_body_max(200.f, 24.f, 80.f, 90.f, 20.f, 60.f), 60.f));   // never below the minimum
        /* wrapping button rows (dialog_same_line) */
        CHECK(fits_on_line(100.f, 8.f, 200.f, 308.f));                     // exactly fits: same line
        CHECK(fits_on_line(100.f, 8.f, 200.4f, 308.f));                    // sub-pixel rounding still fits
        CHECK(!fits_on_line(100.f, 8.f, 201.f, 308.f));                    // one pixel too wide: next line
        CHECK(!fits_on_line(400.f, 8.f, 10.f, 300.f));                     // already past the edge
        float x = 20.f; int rows = 1;                                       // five 150 px buttons in a 500 px row: 3 + 2
        for (int i = 0; i < 5; ++i) { if (i && !fits_on_line(x, 8.f, 150.f, 520.f)) { ++rows; x = 20.f; } x += (x > 20.f ? 8.f : 0.f) + 150.f; }
        CHECK(rows == 2);
    }

    /* ---- option columns ---- */
    {
        RowIn in;
        in.gap = 16.f; in.ctl_min = 176.f; in.label_min = 160.f;
        /* 1. everything fits */
        in.avail = 1200.f; in.label_w = 330.f; in.ctl_w = 400.f;
        RowOut o = option_columns(in);
        CHECK(!o.wrap && near(o.ctl_w, 400.f) && near(o.ctl_x, 800.f) && near(o.label_w, 784.f));
        CHECK(o.label_w >= in.label_w);                                   // the longest label is never clipped
        /* 2. the control shrinks, labels stay on one line */
        in.avail = 700.f;
        o = option_columns(in);
        CHECK(!o.wrap && near(o.ctl_w, 354.f) && near(o.label_w, 330.f) && o.ctl_w >= in.ctl_min);
        /* 3. both give way */
        in.avail = 480.f;
        o = option_columns(in);
        CHECK(!o.wrap && near(o.ctl_w, 216.f) && near(o.label_w, 248.f) && o.label_w >= in.label_min);
        /* 4. too narrow: the control wraps under the label */
        in.avail = 330.f;
        o = option_columns(in);
        CHECK(o.wrap && near(o.ctl_x, 0.f) && near(o.ctl_w, 330.f) && near(o.label_w, 330.f));
        /* a short control never stretches */
        in.avail = 300.f; in.label_w = 600.f; in.ctl_w = 120.f;
        o = option_columns(in);
        CHECK(o.ctl_w <= 120.f);
        /* columns never overlap and stay inside the row */
        for (float a = 0.f; a <= 1600.f; a += 13.f)
            for (float lw = 40.f; lw <= 600.f; lw += 70.f)
                for (float cw = 60.f; cw <= 600.f; cw += 90.f) {
                    RowIn q; q.avail = a; q.label_w = lw; q.ctl_w = cw; q.gap = 16.f; q.ctl_min = 176.f; q.label_min = 160.f;
                    RowOut r = option_columns(q);
                    CHECK(r.ctl_w <= cw + 0.01f && r.ctl_w >= 0.f);
                    CHECK(r.ctl_x >= -0.01f && r.ctl_x + r.ctl_w <= a + 0.01f);
                    if (!r.wrap) CHECK(r.label_w + q.gap <= r.ctl_x + 0.01f);
                    if (!r.wrap && lw + 16.f + cw <= a) CHECK(near(r.ctl_w, cw) && r.label_w >= lw - 0.01f);
                }
    }

    /* ---- steppers, lists, tabs ---- */
    CHECK(step_value(3, 0, 4, 1, +1, false) == 4);
    CHECK(step_value(4, 0, 4, 1, +1, false) == 4);
    CHECK(step_value(4, 0, 4, 1, +1, true) == 0);
    CHECK(step_value(0, 0, 4, 1, -1, true) == 4);
    CHECK(step_value(0, 0, 4, 1, -1, false) == 0);
    CHECK(step_value(8, 0, 10, 5, +1, true) == 10);                     // overshoot lands on the end first
    CHECK(step_value(10, 0, 10, 5, +1, true) == 0);                     // then wraps
    CHECK(step_value(2, 10, 0, 0, +1, false) == 3);                     // swapped bounds, step 0 -> 1
    CHECK(tab_step(0, 4, +1, false) == 1);
    CHECK(tab_step(3, 4, +1, false) == -1);
    CHECK(tab_step(3, 4, +1, true) == 0);
    CHECK(tab_step(0, 4, -1, true) == 3);
    CHECK(tab_step(0, 1, +1, true) == -1);                              // a single tab never "switches"
    CHECK(tab_step(9, 4, -1, false) == 2);                              // out-of-range current clamps first
    CHECK(tab_step(0, 0, +1, true) == -1);
    CHECK(list_move_blocked(0, 5, -1) && !list_move_blocked(0, 5, +1));
    CHECK(list_move_blocked(4, 5, +1) && !list_move_blocked(4, 5, -1));
    CHECK(list_move_blocked(0, 0, +1));

    if (fails) { std::printf("hub_uikit_test: %d failure(s)\n", fails); return 1; }
    std::printf("hub_uikit_test: all checks passed\n");
    return 0;
}
