#include "lib_compact_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui::lc;

    /* compact threshold: 1080p and 720p-tall-enough never compact; short or narrow do */
    CHECK(!compact_top(1080.f, 1700.f, 8.f));
    CHECK(!compact_top(800.f, 1100.f, 8.f));
    CHECK(!compact_top(760.f, 1100.f, 8.f));
    CHECK(compact_top(759.f, 1100.f, 8.f));
    CHECK(compact_top(720.f, 1100.f, 8.f));
    CHECK(compact_top(600.f, 890.f, 8.f));
    CHECK(compact_top(900.f, 799.f, 8.f));
    CHECK(!compact_top(900.f, 801.f, 8.f));
    CHECK(!compact_top(900.f, 1100.f, 8.f) && compact_top(900.f, 1100.f, 16.f));   // big UI scale makes the same pixels narrow

    /* plenty of room: nothing collapses */
    Plan p = plan_row(1700.f, 600.f, 130.f, 130.f, 40.f, 190.f, 40.f, 136.f, 2);
    CHECK(p.level == 0 && p.fits && !p.friends_icon() && !p.sort_icon() && !p.add_icon());

    /* a little short: friends first, then sort */
    p = plan_row(1000.f, 520.f, 130.f, 130.f, 40.f, 190.f, 40.f, 136.f, 2);   // 1000-520-260-190 = 30 < 136
    CHECK(p.level == 2 && p.fits);
    p = plan_row(1100.f, 520.f, 130.f, 130.f, 40.f, 190.f, 40.f, 136.f, 2);   // lvl1: 1100-520-170-190 = 220 fits
    CHECK(p.level == 1 && p.friends_icon() && !p.sort_icon());

    /* compact bar may also shrink Add games */
    p = plan_row(890.f, 520.f, 130.f, 130.f, 40.f, 190.f, 40.f, 136.f, 3);
    CHECK(p.level == 3 && p.fits && p.add_icon());

    /* hopeless: highest level, not fitting */
    p = plan_row(300.f, 520.f, 130.f, 130.f, 40.f, 190.f, 40.f, 136.f, 3);
    CHECK(p.level == 3 && !p.fits);
    p = plan_row(300.f, 520.f, 130.f, 130.f, 40.f, 190.f, 40.f, 136.f, 2);
    CHECK(p.level == 2 && !p.fits);

    CHECK(chips_row_h(30.f, 6.f, false) == 30.f);
    CHECK(chips_row_h(30.f, 6.f, true) == 36.f);

    CHECK(body_room(660.f, 400.f, 100.f, 24.f, 60.f) == 136.f);
    CHECK(body_room(660.f, 600.f, 100.f, 24.f, 60.f) == 60.f);   // never below the floor

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_libcompact_test ok\n");
    return 0;
}
