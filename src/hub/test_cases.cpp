#include "cases_logic.h"
#include "cases_stage.h"
#include "logic.h"

#include <cmath>
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui;
    using namespace hubui::cases;
    // ---- slot pose ----
    SlotPose c = slot_pose(0.f);
    CHECK(c.x == 0.f && c.yaw == 0.f && c.z == 0.f && c.scale == 1.f && c.alpha == 1.f);
    SlotPose r1 = slot_pose(1.f), l1 = slot_pose(-1.f), r5 = slot_pose(5.f);
    CHECK(std::fabs(r1.yaw - 0.72f) < 0.12f && r1.yaw > 0.f && l1.yaw < 0.f);    // the neighbour is angled, partly turned (~41 degrees)
    CHECK(std::fabs(r1.scale - 0.82f) < 0.03f);                                  // and noticeably smaller than the centre
    CHECK(r5.yaw > 1.28f && r5.yaw < 1.45f);                                       // further out: close to side-on, spines show
    CHECK(slot_pose(-5.f).yaw == -r5.yaw && slot_pose(-5.f).x == -r5.x);         // mirrored
    // x, yaw and z only grow with the distance; far cases are packed tight (small pitch)
    float px = -1, py = -1, pz = -1;
    for (float a = 0; a <= 12.2f; a += 0.05f) {
        SlotPose p = slot_pose(a);
        CHECK(p.x >= px - 1e-5f && p.yaw >= py - 1e-5f && p.z >= pz - 1e-5f);
        px = p.x; py = p.yaw; pz = p.z;
        CHECK(p.yaw < 1.5f && p.scale > 0.7f && p.alpha >= 0.f && p.alpha <= 1.f);
    }
    CHECK(std::fabs((slot_pose(7.f).x - slot_pose(6.f).x) - 0.16f) < 0.001f);
    CHECK(slot_pose(1.f).x - slot_pose(0.f).x > 0.6f && std::fabs(slot_pose(1.f).x - kNeighbourBase) < 1e-4f && slot_pose(3.f).x - slot_pose(2.f).x < 0.2f);
    // smooth: no jumps between close distances
    for (float a = 0.f; a < 6.f; a += 0.01f) CHECK(std::fabs(slot_pose(a + 0.01f).x - slot_pose(a).x) < 0.02f && std::fabs(slot_pose(a + 0.01f).yaw - slot_pose(a).yaw) < 0.02f);
    // edge fade: opaque near, zero at the edge of the window (nothing pops in)
    CHECK(slot_pose(8.f).alpha == 1.f && slot_pose(12.2f).alpha == 0.f && slot_pose(kWindow + 1.f).alpha == 0.f);
    CHECK(slot_pose(10.f).alpha > 0.f && slot_pose(10.f).alpha < 1.f);

    // ---- case shapes ----
    using shelf::Media;
    for (int i = 0; i < shelf::kMediaCount; ++i) {
        CaseDims d = case_dims((Media)i);
        CHECK(d.w > 0.3f && d.h > 0.5f && d.h <= 1.0f && d.d > 0.04f && d.d < 0.2f);
        CHECK(is_box((Media)i) == shelf::is_cart((Media)i));
        CHECK(has_sheen((Media)i) == !is_box((Media)i));
    }
    CHECK(kind_for(Media::GcCase) == CaseKind::Keep && kind_for(Media::Ps2Case) == CaseKind::Keep && kind_for(Media::WiiUCase) == CaseKind::Keep);
    CHECK(kind_for(Media::DsCase) == CaseKind::Small && kind_for(Media::PspCase) == CaseKind::Small && kind_for(Media::N3dsCase) == CaseKind::Small);
    CHECK(kind_for(Media::Ps1Jewel) == CaseKind::Jewel && kind_for(Media::N64Cart) == CaseKind::Box && kind_for(Media::GbaCart) == CaseKind::Box);
    CHECK(case_dims(Media::DsCase).w < case_dims(Media::GcCase).w && case_dims(Media::DsCase).h < case_dims(Media::GcCase).h);   // small cases are smaller
    CHECK(case_dims(Media::N64Cart).d > case_dims(Media::GcCase).d);                                                            // boxes are thicker
    CHECK(case_dims(Media::GcCase).w / case_dims(Media::GcCase).h > 0.68f && case_dims(Media::GcCase).w / case_dims(Media::GcCase).h < 0.74f);   // 135 x 190 mm

    // wide boxes push their neighbours out so nothing overlaps the centre face; keep cases fit at the base distance
    {
        const CaseDims dvd = case_dims(Media::GcCase), box = case_dims(Media::N64Cart);
        const float a = neighbour_x(dvd, display_scale(Media::GcCase), dvd, display_scale(Media::GcCase));
        const float b = neighbour_x(box, display_scale(Media::N64Cart), box, display_scale(Media::N64Cart));
        CHECK(a <= kNeighbourBase + 0.03f && b > kNeighbourBase + 0.1f && b > a);
        CHECK(display_scale(Media::GcCase) == 1.f && display_scale(Media::DsCase) > 1.f && display_scale(Media::DsCase) < 1.f / case_dims(Media::DsCase).h);
        for (int i = 0; i < shelf::kMediaCount; ++i) CHECK(case_dims((Media)i).h * display_scale((Media)i) <= 1.0f + 1e-4f);
    }

    // ---- stepping: wrap and clamp at the ends ----
    CHECK(step_index(3, 1, 10, true) == 4 && step_index(3, -1, 10, true) == 2);
    CHECK(step_index(9, 1, 10, true) == 0 && step_index(0, -1, 10, true) == 9);       // wraps
    CHECK(step_index(9, 1, 10, false) == 9 && step_index(0, -1, 10, false) == 0);     // clamps
    CHECK(step_index(0, 1, 1, true) == 0 && step_index(5, 1, 0, true) == 0 && step_index(99, 0, 10, false) == 9);
    CHECK(clamp_index(-4, 10) == 0 && clamp_index(40, 10) == 9 && clamp_index(5, 0) == 0);
    CHECK(hold_boost(0.f) == 1 && hold_boost(1.f) == 2 && hold_boost(2.f) == 3 && hold_boost(9.f) == 5);
    int f = 0, l = 0;
    window(0.f, 2000, &f, &l);   CHECK(f == 0 && l == kWindow);
    window(1000.4f, 2000, &f, &l); CHECK(f == 1000 - kWindow && l == 1001 + kWindow && l - f < 2 * kWindow + 3);
    window(1999.f, 2000, &f, &l); CHECK(l == 1999 && f == 1999 - kWindow);
    window(0.f, 0, &f, &l);      CHECK(f > l);
    CHECK(jump_start(5.f, 6, 6.f) == 5.f && jump_start(0.f, 100, 6.f) == 94.f && jump_start(100.f, 0, 6.f) == 6.f);

    // ---- the spring settles on its target, snaps when motion is reduced ----
    shelf::Spring sp;
    int frames = 0;
    while (pos_step(sp, 5.f, 1.f / 60.f) && frames < 600) ++frames;
    CHECK(sp.x == 5.f && sp.v == 0.f && frames > 8 && frames < 120);
    shelf::Spring sq;
    pos_step(sq, 40.f, 1.f / 60.f, true);
    CHECK(sq.x == 40.f && sq.v == 0.f);
    shelf::Spring sr;
    float maxx = 0;
    frames = 0;
    while (pos_step(sr, 1.f, 1.f / 60.f) && frames < 600) { maxx = std::max(maxx, sr.x); ++frames; }
    CHECK(maxx < 1.1f);   // a hint of overshoot at most
    shelf::Spring ss;
    frames = 0;
    while (pos_step(ss, 300.f, 1.f / 60.f) && frames < 900) ++frames;   // a long run still arrives quickly
    CHECK(ss.x == 300.f && frames < 200);

    // ---- tabs ----
    std::map<std::string, int> counts = {{"gba", 3}, {"n64", 5}, {"zzz", 1}, {"gc", 0}, {"ps2", 2}};
    std::vector<Tab> tabs = build_tabs(counts, 11, "");
    CHECK(tabs.size() == 5 && tabs[0].sys.empty() && tabs[0].label == "All" && tabs[0].count == 11);
    CHECK(tabs[1].sys == "n64" && tabs[2].sys == "ps2" && tabs[3].sys == "gba" && tabs[4].sys == "zzz" && tabs[4].label == "ZZZ");   // library order, unknown last, empty dropped
    CHECK(build_tabs(counts, 11, "gc").size() == 6);   // the active console keeps its tab even when a filter empties it
    CHECK(build_tabs({}, 0, "").size() == 1);
    CHECK(tab_index(tabs, "gba") == 3 && tab_index(tabs, "nope") == 0 && tab_index(tabs, "") == 0);
    CHECK(tab_step(0, 1, 5) == 1 && tab_step(4, 1, 5) == 0 && tab_step(0, -1, 5) == 4 && tab_step(0, 1, 0) == 0);
    TabMemory mem;
    CHECK(mem.recall("n64").empty());
    mem.remember("", "n64", "fake:7");          // browsing All, resting on an N64 game
    CHECK(mem.recall("n64") == "fake:7" && mem.recall("") == "fake:7");
    mem.remember("gba", "gba", "fake:12");
    CHECK(mem.recall("gba") == "fake:12" && mem.recall("n64") == "fake:7" && mem.recall("") == "fake:12");
    mem.remember("n64", "n64", "");             // an empty key never overwrites
    CHECK(mem.recall("n64") == "fake:7");
    std::vector<std::string> keys = {"fake:3", "fake:7", "fake:9"};
    CHECK(land_index(keys, "fake:7") == 1 && land_index(keys, "fake:99") == 0 && land_index(keys, "") == 0 && land_index({}, "x") == 0);

    // ---- flip / inspect / back ----
    Mode m = Mode::Browse;
    m = on_flip_tap(m);  CHECK(m == Mode::Flip && ModeState{m}.flip_target() > 3.1f);
    m = on_flip_tap(m);  CHECK(m == Mode::Browse && ModeState{m}.flip_target() == 0.f);
    m = on_inspect(m);   CHECK(m == Mode::Inspect);
    m = on_flip_tap(m);  CHECK(m == Mode::Browse);   // X leaves inspect
    m = on_flip_tap(m);  m = on_inspect(m);  CHECK(m == Mode::Flip);   // a flipped case is not inspected
    CHECK(on_back(&m) && m == Mode::Browse && !on_back(&m));   // B leaves flip, and only then falls through to "back"
    m = Mode::Inspect;   CHECK(on_back(&m) && m == Mode::Browse);
    CHECK(on_move(Mode::Flip) == Mode::Browse && on_move(Mode::Inspect) == Mode::Browse);
    Spin s = spin_apply(Spin(), 1.5f, 2.f);
    CHECK(s.yaw == 1.5f && s.pitch <= 0.55f + 1e-6f);
    s = spin_apply(s, -0.5f, -9.f);
    CHECK(s.pitch >= -0.55f - 1e-6f && std::fabs(spin_home(6.4f) - 6.28318531f) < 1e-4f && spin_home(0.5f) == 0.f);

    // ---- shuffle ----
    std::vector<ShuffleCand> cand;
    const int64_t now = 1700000000;
    cand.push_back({"a", now - 3600, false});        // played an hour ago: not a candidate
    cand.push_back({"b", 0, false});                 // never played
    cand.push_back({"c", now - 400 * 86400, false});
    for (double u : {0.0, 0.3, 0.6, 0.99}) { int i = pick_shuffle(cand, 1, now, u); CHECK(i == 2); }   // not "a" (recent), not the current "b"
    for (double u : {0.0, 0.5, 0.99}) { int i = pick_shuffle(cand, 0, now, u); CHECK(i == 1 || i == 2); }
    CHECK(pick_shuffle({}, 0, now, 0.5) == -1 && pick_shuffle({cand[0]}, 0, now, 0.5) == 0);
    std::vector<ShuffleCand> recent = {{"x", now - 10, false}, {"y", now - 20, false}};
    CHECK(pick_shuffle(recent, 0, now, 0.4) == 1);   // everything recent: any game but the current one

    // ---- subtitle ----
    CHECK(subtitle_text("Nintendo 64", 1999, 4) == "Nintendo 64  -  1999  -  1-4 players");
    CHECK(subtitle_text("GBA", 0, 1) == "GBA  -  1 player" && subtitle_text("PS2", 2004, 0) == "PS2  -  2004" && subtitle_text("DS", 0, 0) == "DS");

    // ---- layout key round-trip + migration ----
    CHECK(kLibLayouts == 6 && (int)LibLayout::Cases == 5);
    CHECK(std::string(layout_key(LibLayout::Cases)) == "cases" && std::string(layout_name(LibLayout::Cases)) == "Cases");
    CHECK(layout_from_key("cases") == LibLayout::Cases);
    for (int i = 0; i < kLibLayouts; ++i) CHECK(layout_from_key(layout_key((LibLayout)i)) == (LibLayout)i);
    CHECK(layout_from_key("shelf") == LibLayout::Shelf && layout_from_key("grid") == LibLayout::Grid && layout_from_key("") == LibLayout::Grid && layout_from_key("Cases") == LibLayout::Grid);   // unknown / future keys fall back to Grid
    Settings st;
    st.lib_layout = "cases";
    st.tv_cases = true;
    Settings back;
    CHECK(settings_from_json(settings_to_json(st), &back));
    CHECK(back.lib_layout == "cases" && back.tv_cases && !Settings().tv_cases);
    Settings old;
    CHECK(settings_from_json("{\"lib_layout\": \"shelf\"}", &old) && old.lib_layout == "shelf" && !old.tv_cases);   // an older file stays valid


    /* car-stage */
    {   // ambient colour from a cover
        Rgb8 a0, a1;
        CHECK(!ambient_from_art({128, 128, 128}, &a0, &a1));                    // grey art: the console colours stay
        CHECK(!ambient_from_art({4, 3, 3}, &a0, &a1));                          // near black too
        CHECK(ambient_from_art({220, 30, 30}, &a0, &a1));                       // a red cover: a deep red and a lighter red, never garish
        float h0, s0, v0, h1, s1, v1;
        rgb_to_hsv(a0, &h0, &s0, &v0); rgb_to_hsv(a1, &h1, &s1, &v1);
        CHECK((h0 < 0.04f || h0 > 0.96f) && v0 < 0.7f && v1 > v0 && s0 <= 0.83f && s0 >= 0.4f && s1 <= s0 + 1e-3f);
        for (int hue = 0; hue < 360; hue += 15) {                               // every hue stays within the tasteful brightness range
            Rgb8 in = hsv_to_rgb((float)hue / 360.f, 1.f, 1.f), o0, o1;
            CHECK(ambient_from_art(in, &o0, &o1));
            float hh, ss, vv;
            rgb_to_hsv(o0, &hh, &ss, &vv);
            CHECK(vv > 0.5f && vv < 0.66f && ss >= 0.4f && ss <= 0.83f);
        }
        Rgb8 rt = hsv_to_rgb(0.33f, 0.5f, 0.8f); float hh, ss, vv; rgb_to_hsv(rt, &hh, &ss, &vv);
        CHECK(std::fabs(hh - 0.33f) < 0.01f && std::fabs(ss - 0.5f) < 0.01f && std::fabs(vv - 0.8f) < 0.01f);
    }
    {   // centre_adj: the enlarged flipped / inspected case comes forward and grows
        CHECK(centre_adj(0.f, 0.f).z == 0.f && centre_adj(0.f, 0.f).scale_mul == 1.f);
        CHECK(centre_adj(3.14159f, 0.f).z < -0.35f && centre_adj(3.14159f, 0.f).scale_mul > 1.09f);
        CHECK(centre_adj(0.f, 1.f).z < -0.5f && centre_adj(0.f, 1.f).scale_mul > 1.04f);
    }
    {   // nothing may intersect the centre case at the flip / inspect pose: every case kind against every neighbour kind, both sides, all angles
        const float dist = 6.5f, eye = 0.55f;
        (void)eye;
        const shelf::Media all[] = {shelf::Media::NesCart, shelf::Media::SnesCart, shelf::Media::N64Cart, shelf::Media::GbCart, shelf::Media::GbcCart, shelf::Media::GbaCart,
                                    shelf::Media::DsCase, shelf::Media::N3dsCase, shelf::Media::Ps1Jewel, shelf::Media::Ps2Case, shelf::Media::GcCase, shelf::Media::WiiUCase,
                                    shelf::Media::PspCase, shelf::Media::Generic};
        struct Ext { float lo, hi; };
        // exact screen x extent (units of U, from the centre line) of a case: its 8 corners turned by yaw / pitch, then perspective-projected
        auto extent = [&](const CaseDims& d, float yaw, float pitch, float scale, float ox, float oz) {
            Ext e{1e9f, -1e9f};
            const float ct = std::cos(yaw), st = std::sin(yaw), cp = std::cos(pitch), sp_ = std::sin(pitch);
            for (int i = 0; i < 8; ++i) {
                const float lx = (i & 1 ? 0.5f : -0.5f) * d.w * scale, ly = ((i & 2) ? 0.5f : -0.5f) * d.h * scale, lz = (i & 4 ? 0.5f : -0.5f) * d.d * scale;
                const float xr = lx * ct + lz * st, zr = -lx * st + lz * ct;
                const float zz = ly * sp_ + zr * cp;
                const float sx = (ox + xr) * dist / std::max(0.4f, dist + oz + zz);
                e.lo = std::min(e.lo, sx); e.hi = std::max(e.hi, sx);
            }
            return e;
        };
        int bad = 0, tested = 0;
        for (shelf::Media mc : all) for (shelf::Media mn : all) {
            const CaseDims dc = case_dims(mc), dn = case_dims(mn);
            const float scc = display_scale(mc), snn = display_scale(mn);
            const float extra = std::max(0.f, neighbour_x(dc, scc, dn, snn) - kNeighbourBase);
            const SlotPose s1 = slot_pose(1.f);
            const float nsc = s1.scale * snn, base = s1.x + extra;
            for (int step = 0; step <= 14; ++step) {
                float fl = 3.14159f * (float)step / 14.f, ins = 0.f;
                for (int mode = 0; mode < 2; ++mode) {
                    if (mode == 1) { fl = 0.f; ins = (float)step / 14.f; }
                    for (float settle : {-0.8f, 0.f, 1.2f}) {   // the bounce of a case coming to rest scales the centre case too
                    if (settle != 0.f && step % 3) continue;
                    const CentreAdj a = centre_adj(fl, ins, settle);
                    for (int sw = 0; sw < 8; ++sw) {   // the free spin of inspect (and pitch); during a flip the yaw is the flip angle
                        const float yaw = mode == 1 ? 6.2831853f * (float)sw / 8.f : fl - 0.13f, pitch = mode == 1 ? ((sw & 1) ? 0.55f : (sw & 2) ? -0.55f : 0.f) * ins : 0.f;
                        const float cs = scc * a.scale_mul;
                        const float now = flip_push(dc, cs, 0.f, a.z, std::fabs(pitch), dn, s1.yaw, nsc, s1.z, base, dist);
                        const float rest = flip_push(dc, scc, 0.f, 0.f, 0.f, dn, s1.yaw, nsc, s1.z, base, dist);
                        const float push = std::max(0.f, now - rest);
                        const Ext ce = extent(dc, yaw, pitch, cs, 0.f, a.z);
                        const Ext re = extent(dn, s1.yaw, 0.f, nsc, base + push, s1.z);      // right neighbour
                        const Ext le = extent(dn, -s1.yaw, 0.f, nsc, -(base + push), s1.z);  // left neighbour (mirror)
                        ++tested;
                        if (re.lo < ce.hi - 1e-4f || le.hi > ce.lo + 1e-4f) { if (++bad <= 3) std::printf("overlap: centre %d neighbour %d fl=%.2f ins=%.2f yaw=%.2f: gap %.3f\n", (int)mc, (int)mn, fl, ins, yaw, re.lo - ce.hi); }
                        // on a 16:9 or 16:10 screen (case height = 0.42 of the width at most) the pushed neighbour must still start on screen
                        for (float aspect : {16.f / 9.f, 16.f / 10.f}) {
                            const float Wd = 1600.f, Hd = Wd / aspect, U = std::min(Hd * 0.42f, Wd * 0.42f);
                            CHECK(re.lo * U < Wd * 0.5f);
                        }
                    }
                    }
                }
            }
        }
        CHECK(bad == 0);
        CHECK(tested > 1000);
    }
    {   // the first neighbours and everything beyond move out by the same amount, so the shelf pitch between them is untouched while a case is flipped
        const float ex = 0.12f, pu = 0.31f;
        CHECK(neighbour_shift(1.f, ex, pu) == neighbour_shift(2.f, ex, pu) && neighbour_shift(2.f, ex, pu) == neighbour_shift(7.f, ex, pu));
        CHECK(neighbour_shift(0.f, ex, pu) == 0.f && neighbour_shift(0.5f, ex, pu) > 0.f && neighbour_shift(0.5f, ex, pu) < neighbour_shift(1.f, ex, pu));
        CHECK(std::fabs((slot_pose(2.f).x + neighbour_shift(2.f, ex, pu) - (slot_pose(1.f).x + neighbour_shift(1.f, ex, pu))) - (slot_pose(2.f).x - slot_pose(1.f).x)) < 1e-6f);
        CHECK(centre_adj(0.f, 0.f, 1.f).scale_mul > 1.029f && centre_adj(0.f, 0.f, 99.f).scale_mul < 1.05f);   // the settle term is bounded
        // a clock running for 30 days still moves the idle float frame to frame
        CHECK(idle_bob(2.6e6 + 0.00) != idle_bob(2.6e6 + 0.5));
    }
    {   // idle float stays tiny and bounded
        float mx = 0.f, my = 0.f;
        for (int i = 0; i < 2000; ++i) { mx = std::max(mx, std::fabs(idle_bob(0.05f * (float)i))); my = std::max(my, std::fabs(idle_sway(0.05f * (float)i))); }
        CHECK(mx < 0.02f && mx > 0.008f && my < 0.02f);
        CHECK(console_chip("GameCube", false) == "GameCube" && console_chip("Switch", true) == "Switch 2");
        CHECK(chip_wants_dark_ink(0.9f) && !chip_wants_dark_ink(0.3f));
    }

    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("hub_cases_test: all ok\n");
    return 0;
}
