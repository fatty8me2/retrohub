#include "shelf_logic.h"

#include <cmath>
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui::shelf;
    // media per system
    CHECK(media_for("nes") == Media::NesCart && media_for("snes") == Media::SnesCart && media_for("n64") == Media::N64Cart);
    CHECK(media_for("gb") == Media::GbCart && media_for("gbc") == Media::GbcCart && media_for("gba") == Media::GbaCart);
    CHECK(media_for("nds") == Media::DsCase && media_for("3ds") == Media::N3dsCase && media_for("ps1") == Media::Ps1Jewel);
    CHECK(media_for("ps2") == Media::Ps2Case && media_for("gc") == Media::GcCase && media_for("wiiu") == Media::WiiUCase && media_for("psp") == Media::PspCase);
    CHECK(media_for("zzz") == Media::Generic && media_for("") == Media::Generic);
    for (int i = 0; i < kMediaCount; ++i) {
        Media m = (Media)i;
        Dims d = base_dims(m);
        CHECK(d.w > 0.2f && d.h > 0.3f && d.h <= 1.0f && d.d > 0.05f);
        CHECK(is_cart(m) == (d.rest_theta == 0.f));
        CHECK(rest_width(d) == (is_cart(m) ? d.w : d.d));
    }
    CHECK(std::string(media_key(Media::Ps1Jewel)) == "ps1-jewel");
    Dims a = item_dims(Media::Ps2Case, hash32("A")), b = item_dims(Media::Ps2Case, hash32("A")), c = item_dims(Media::Ps2Case, hash32("Some other game"));
    CHECK(a.d == b.d && a.h == b.h);
    CHECK(c.h <= 1.0f && c.d > 0.1f && c.d < 0.19f);

    // packing: sections [5, 2, 4], slot 40 px, row 100 px wide, margin 10 -> 2 per row
    std::vector<float> w(11, 40.f);
    Layout l = pack_rows(w, {5, 2, 4}, 100.f, 10.f, 50.f, 20.f, 5.f);
    CHECK(l.L.rows.size() == 6);   // 5 -> 3 rows (2,2,1), 2 -> 1 row, 4 -> 2 rows
    CHECK(l.L.rows[0].count == 2 && l.L.rows[2].count == 1 && l.L.rows[3].first == 5 && l.L.rows[3].count == 2 && l.L.rows[4].first == 7);
    CHECK(l.row_first[0] == 1 && l.row_first[1] == 0 && l.row_first[3] == 1 && l.row_first[4] == 1 && l.row_first[5] == 0);
    CHECK(l.row_sec[2] == 0 && l.row_sec[3] == 1 && l.row_sec[5] == 2);
    CHECK(l.x[0] == 10.f && l.x[1] == 50.f && l.x[2] == 10.f);
    CHECK(l.L.item_row[4] == 2 && l.L.item_row[10] == 5);
    CHECK(l.L.rows[0].y == 20.f && l.L.rows[1].y == 70.f && l.L.total_h == 20.f + 6 * 50.f + 5.f);
    for (size_t r = 0; r < l.L.rows.size(); ++r) {   // no row runs past the right margin
        const auto& row = l.L.rows[r];
        CHECK(l.x[row.first + row.count - 1] + 40.f <= 90.f + 1e-3f);
    }
    // an item wider than the row sits alone; empty input is fine
    Layout big = pack_rows({300.f, 20.f, 20.f}, {3}, 100.f, 10.f, 50.f, 0.f, 0.f);
    CHECK(big.L.rows.size() == 2 && big.L.rows[0].count == 1 && big.L.rows[1].count == 2);
    Layout none = pack_rows({}, {}, 100.f, 10.f, 50.f, 20.f, 5.f);
    CHECK(none.L.rows.empty() && none.L.total_h == 25.f && move_focus(none, 0, Dir::Left) == 0 && row_of(none, 3) == 0);
    Layout zero = pack_rows(w, {0, 11, 0}, 100.f, 10.f, 50.f, 0.f, 0.f);   // empty sections make no rows
    CHECK(zero.L.rows.size() == 6 && zero.row_sec[0] == 1);

    // visible range
    int f = -2, la = -2;
    visible_rows(l, 71.f, 168.f, &f, &la);
    CHECK(f == 1 && la == 2);
    visible_rows(l, 1000.f, 1100.f, &f, &la);
    CHECK(f == -1 && la == -1);
    visible_rows(l, 0.f, 2000.f, &f, &la);
    CHECK(f == 0 && la == 5);

    // focus movement
    CHECK(move_focus(l, 0, Dir::Left) == 0);                 // first item: stays
    CHECK(move_focus(l, 1, Dir::Left) == 0);
    CHECK(move_focus(l, 2, Dir::Left) == 1);                 // wraps to the end of the shelf above
    CHECK(move_focus(l, 1, Dir::Right) == 2);                // and from the end of a shelf to the next one's start
    CHECK(move_focus(l, 4, Dir::Right) == 5);                // across a section
    CHECK(move_focus(l, 10, Dir::Right) == 10);              // last item: stays
    CHECK(move_focus(l, 0, Dir::Up) == 0 && move_focus(l, 1, Dir::Up) == 1);   // top row: nowhere to go
    CHECK(move_focus(l, 10, Dir::Down) == 10);
    CHECK(move_focus(l, 0, Dir::Down) == 2 && move_focus(l, 1, Dir::Down) == 3);
    CHECK(move_focus(l, 3, Dir::Up) == 1 && move_focus(l, 2, Dir::Up) == 0);
    CHECK(move_focus(l, 4, Dir::Down) == 5);
    CHECK(move_focus(l, -1, Dir::Right) == -1 && move_focus(l, 99, Dir::Left) == 99);
    // varied widths: up/down pick by centre
    Layout v = pack_rows({100.f, 20.f, 20.f, 20.f, 60.f, 60.f}, {1, 3, 2}, 400.f, 0.f, 10.f, 0.f, 0.f);
    CHECK(v.L.rows.size() == 3);
    CHECK(move_focus(v, 0, Dir::Down) == 3);                 // centre 50 -> the item centred at 50
    CHECK(move_focus(v, 5, Dir::Up) == 3);                   // centre 90 -> nearest centre above is 50
    CHECK(move_focus(v, 4, Dir::Up) == 2);                   // centre 30 -> item centred at 30

    // spring: converges, never NaN, snaps in reduced motion, safe with a huge dt
    Spring s;
    float peak = 0;
    for (int i = 0; i < 240; ++i) { spring_step(s, 1.f, 1.f / 60.f); peak = std::fmax(peak, s.x); }
    CHECK(spring_settled(s, 1.f));
    CHECK(peak > 1.0f && peak < 1.2f);                       // a small overshoot, not a wild one
    spring_step(s, 0.f, 100.f);
    CHECK(std::isfinite(s.x) && std::isfinite(s.v));
    for (int i = 0; i < 240; ++i) spring_step(s, 0.f, 1.f / 60.f);
    CHECK(spring_settled(s, 0.f));
    Spring r;
    spring_step(r, 1.f, 1.f / 60.f, true);
    CHECK(r.x == 1.f && r.v == 0.f);
    Spring z;
    spring_step(z, 1.f, 0.f);
    CHECK(z.x == 0.f);
    CHECK(slide_target(true, false) == 1.f && slide_target(false, true) > 0.f && slide_target(false, true) < 1.f && slide_target(false, false) == 0.f && slide_target(true, true) == 1.f);

    // pose
    Dims cd = base_dims(Media::Ps2Case), kd = base_dims(Media::N64Cart);
    SlidePose p0 = slide_pose(Media::Ps2Case, cd, 0.f, 0.f), p1 = slide_pose(Media::Ps2Case, cd, 1.f, 0.f);
    CHECK(p0.scale == 1.f && p0.lift == 0.f && std::fabs(p0.theta - cd.rest_theta) < 1e-5f);
    CHECK(p1.scale > 1.2f && p1.lift > 0.f && p1.theta > cd.rest_theta + 0.8f && p1.theta < 0.f);   // the spine swung toward the cover
    SlidePose q0 = slide_pose(Media::N64Cart, kd, 0.f, 0.f), q1 = slide_pose(Media::N64Cart, kd, 1.f, 0.f);
    CHECK(q0.scale == 1.f && q1.scale > 1.2f && q1.theta < q0.theta);
    SlidePose ds = slide_pose(Media::DsCase, base_dims(Media::DsCase), 1.f, 0.f);
    CHECK(ds.card > 0.9f && p1.card == 0.f && q1.card == 0.f);
    SlidePose rm = slide_pose(Media::Ps2Case, cd, 1.4f, 30.f, true);   // reduced motion: no overshoot, no lean
    CHECK(rm.scale == p1.scale && rm.theta == p1.theta);
    SlidePose lean = slide_pose(Media::Ps2Case, cd, 1.f, 30.f);
    CHECK(lean.theta != p1.theta);

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("shelf tests ok\n");
    return 0;
}
