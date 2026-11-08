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
    CHECK(case_dims(Media::DsCase).h < case_dims(Media::GcCase).h && case_dims(Media::PspCase).w < case_dims(Media::GcCase).w);   // small cases are smaller (the DS case is landscape, like its cover scans)
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

    /* car-cases: per-console retail packaging (shapes, kinds, media mapping) */
    {
        using shelf::media_for;
        CHECK(media_for("switch") == Media::SwitchCase && kind_for(Media::SwitchCase) == CaseKind::Keep && has_sheen(Media::SwitchCase) && !is_box(Media::SwitchCase));
        CHECK(std::string(shelf::media_key(Media::SwitchCase)) == "switch-case" && !shelf::is_cart(Media::SwitchCase));
        // every console has its own, non-generic media: no two share one
        const char* all[] = {"nes", "snes", "n64", "gb", "gbc", "gba", "nds", "3ds", "ps1", "ps2", "gc", "wiiu", "psp", "switch"};
        for (const char* sys : all) CHECK(media_for(sys) != Media::Generic);
        for (int i = 0; i < 14; ++i) for (int j = i + 1; j < 14; ++j) CHECK(media_for(all[i]) != media_for(all[j]));
        // kinds: plastic keep cases, a jewel case and cardboard boxes
        CHECK(kind_for(Media::GcCase) == CaseKind::Keep && kind_for(Media::Ps2Case) == CaseKind::Keep && kind_for(Media::WiiUCase) == CaseKind::Keep);
        CHECK(kind_for(Media::Ps1Jewel) == CaseKind::Jewel);
        for (Media m : {Media::NesCart, Media::SnesCart, Media::N64Cart, Media::GbCart, Media::GbcCart, Media::GbaCart}) CHECK(kind_for(m) == CaseKind::Box && !has_sheen(m));
        // proportions: Switch slim and tall, Wii U Blu-ray sized, PS1 wider than tall, DS near square, PSP tall and narrow, SNES / N64 landscape, NES portrait
        const CaseDims sw = case_dims(Media::SwitchCase), wu = case_dims(Media::WiiUCase), gc = case_dims(Media::GcCase), ps2 = case_dims(Media::Ps2Case);
        CHECK(sw.w < wu.w && sw.h < gc.h && sw.d < ps2.d && sw.w / sw.h > 0.55f && sw.w / sw.h < 0.65f);
        CHECK(wu.h < gc.h && wu.d < ps2.d && gc.d < ps2.d);
        CHECK(case_dims(Media::Ps1Jewel).w > case_dims(Media::Ps1Jewel).h);
        CHECK(case_dims(Media::DsCase).w / case_dims(Media::DsCase).h > 1.1f && case_dims(Media::DsCase).w / case_dims(Media::DsCase).h < 1.25f);   // landscape, like the DS cover scans (1.12 w/h)
        CHECK(case_dims(Media::N3dsCase).w == case_dims(Media::DsCase).w && case_dims(Media::N3dsCase).h == case_dims(Media::DsCase).h);
        CHECK(case_dims(Media::PspCase).w / case_dims(Media::PspCase).h < 0.66f && case_dims(Media::PspCase).h > case_dims(Media::DsCase).h);
        CHECK(case_dims(Media::SnesCart).w > case_dims(Media::SnesCart).h && case_dims(Media::N64Cart).w > case_dims(Media::N64Cart).h && case_dims(Media::NesCart).w < case_dims(Media::NesCart).h);
        CHECK(case_dims(Media::GbaCart).d > 0.12f && case_dims(Media::GbCart).h < case_dims(Media::NesCart).h);
        // the switch case is drawn big enough to read, never taller than the stage allows
        CHECK(display_scale(Media::SwitchCase) > 1.f && case_dims(Media::SwitchCase).h * display_scale(Media::SwitchCase) <= 1.f + 1e-4f);
    }

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
    /* car-nav */
    {
        CHECK(nav_label_letter("Zelda") == "Z" && nav_label_letter("alpha") == "A" && nav_label_letter("1942") == "#" && nav_label_letter("!!!") == "#" && nav_label_letter("") == "#");
        CHECK(nav_label_year(1998) == "1998" && nav_label_year(0) == "?");
        const int64_t now = 10000000;
        CHECK(nav_label_age(now, 0) == "Never" && nav_label_age(now, now - 3600) == "Today" && nav_label_age(now, now - 3 * 86400) == "This week");
        CHECK(nav_label_age(now, now - 20 * 86400) == "This month" && nav_label_age(now, now - 90 * 86400) == "Earlier" && nav_label_age(now, 0, false) == "Unknown");
        CHECK(nav_label_plays(0) == "Never" && nav_label_plays(600) == "Under 1h" && nav_label_plays(3600) == "1h+" && nav_label_plays(40000) == "10h+");
        CHECK(nav_kind_for(SortMode::Title) == NavKind::Letters && nav_kind_for(SortMode::ReleaseYear) == NavKind::Years && nav_kind_for(SortMode::Console) == NavKind::Consoles);
        CHECK(nav_kind_for(SortMode::LastPlayed) == NavKind::Recency && nav_kind_for(SortMode::MostPlayed) == NavKind::Plays && nav_kind_for(SortMode::RecentlyAdded) == NavKind::Added);
        // sections: runs of equal labels; an empty label (the pinned game) joins its neighbour
        const std::vector<std::string> lb = {"", "A", "A", "B", "B", "B", "#", "A"};
        const auto sc = build_sections(lb);
        CHECK(sc.size() == 4 && sc[0].label == "A" && sc[0].first == 0 && sc[0].count == 3 && sc[1].label == "B" && sc[1].first == 3 && sc[1].count == 3);
        CHECK(sc[2].label == "#" && sc[2].first == 6 && sc[3].label == "A" && sc[3].first == 7 && sc[3].count == 1);   // the same label again later is a new section
        {   // Title sort puts every digit / symbol / non-ASCII title in one block: a single "#" section
            const auto hs = build_sections({"#", "#", "#", "A", "B", "Z"});
            CHECK(hs.size() == 4 && hs[0].label == "#" && hs[0].count == 3);
        }
        CHECK(build_sections({}).empty() && build_sections({"", ""}).empty());
        CHECK(section_index(sc, 0) == 0 && section_index(sc, 2) == 0 && section_index(sc, 3) == 1 && section_index(sc, 5) == 1 && section_index(sc, 7) == 3);
        // previous / next section
        CHECK(section_step(sc, 0, 1, 8) == 3 && section_step(sc, 4, 1, 8) == 6 && section_step(sc, 7, 1, 8) == 7);   // last section: the last game
        CHECK(section_step(sc, 4, -1, 8) == 3 && section_step(sc, 3, -1, 8) == 0 && section_step(sc, 1, -1, 8) == 0 && section_step(sc, 0, -1, 8) == 0);
        CHECK(section_by_label(sc, "B") == 1 && section_by_label(sc, "Q") == -1);
        CHECK(section_step({}, 5, 1, 100) == 13 && section_step({}, 5, -1, 100) == 0);
        // scrubber position <-> index
        CHECK(scrub_frac(0, 100) == 0.f && scrub_frac(99, 100) == 1.f && scrub_frac(5, 1) == 0.f && std::fabs(scrub_frac(50, 101) - 0.5f) < 1e-6f);
        CHECK(scrub_index(0.f, 100) == 0 && scrub_index(1.f, 100) == 99 && scrub_index(-3.f, 100) == 0 && scrub_index(7.f, 100) == 99 && scrub_index(0.5f, 101) == 50 && scrub_index(0.5f, 0) == 0);
        for (int n : {2, 7, 240, 3000}) for (int i = 0; i < n; i += std::max(1, n / 37)) CHECK(scrub_index(scrub_frac(i, n), n) == i);   // the round trip never drifts
        CHECK(std::fabs(scrub_x(50, 101, 100.f, 300.f) - 200.f) < 1e-3f);
        CHECK(count_text(122, 2400) == "123 / 2,400" && count_text(0, 5) == "1 / 5" && count_text(0, 0) == "0 / 0" && count_text(5, 1234567) == "6 / 1,234,567");
        // typed prefix
        const std::vector<std::string> ti = {"Alpha", "bravo", "Bravura", "Charlie"};
        auto tat = [&](int i) { return ti[(size_t)i]; };
        CHECK(prefix_index(4, tat, "br") == 1 && prefix_index(4, tat, "BRAV") == 1 && prefix_index(4, tat, "bravu") == 2 && prefix_index(4, tat, "x") == -1 && prefix_index(4, tat, "") == -1 && prefix_index(4, tat, "charlie2") == -1);
        // hold / wheel
        CHECK(nav_hold_boost(0.f) == 1 && nav_hold_boost(1.f) == 2 && nav_hold_boost(2.f) == 4 && nav_hold_boost(3.f) == 8 && nav_hold_boost(4.f) == 14 && nav_hold_boost(9.f) == 24);
        for (float t = 0.f; t < 9.f; t += 0.1f) CHECK(nav_hold_boost(t + 0.1f) >= nav_hold_boost(t));
        CHECK(nav_wheel_gain(2.f) == 1.f && nav_wheel_gain(5.f) == 1.f && nav_wheel_gain(20.f) == 8.f && nav_wheel_gain(100.f) == 8.f && nav_wheel_gain(12.f) > 1.f && nav_wheel_gain(12.f) < 8.f);
        CHECK(!nav_is_fast(1, 1.f) && nav_is_fast(2, 1.f) && nav_is_fast(1, 3.f));
        // sort picker rows
        SortSpec sp0;
        CHECK(kSortRows == kSortModes + 2 && sort_row_on(sp0, 0) && !sort_row_on(sp0, 4) && !sort_row_on(sp0, kSortModes) && !sort_row_on(sp0, kSortModes + 1));
        SortSpec s1 = sort_apply_row(sp0, (int)SortMode::ReleaseYear);
        CHECK(s1.mode == SortMode::ReleaseYear && !s1.reverse && !s1.favs_first);
        s1 = sort_apply_row(s1, kSortModes);        CHECK(s1.reverse && s1.mode == SortMode::ReleaseYear);
        s1 = sort_apply_row(s1, kSortModes + 1);    CHECK(s1.favs_first && s1.reverse);
        s1 = sort_apply_row(s1, (int)SortMode::Console); CHECK(s1.mode == SortMode::Console && !s1.reverse && s1.favs_first);   // a new mode starts in its natural direction; favorites first stays
        CHECK(sort_apply_row(s1, 99).mode == SortMode::Console);
    }

    {   // flip / inspect fit: the enlarged case (any yaw, pitch up to 0.55) stays between the tab bar and the scrubber strip, for every case kind
        for (float U : {260.f, 340.f, 420.f, 480.f}) {
            FitScene fs; fs.U = U; fs.eye = 0.55f; fs.dist = 6.5f; fs.hy = 540.f - 0.55f * U;   // the floor line (f0) at y = 540
            const float f0 = fs.hy + fs.eye * U, top_lim = f0 - 1.12f * U, bot_lim = f0 + 0.45f * U;
            for (int i = 0; i < shelf::kMediaCount; ++i) {
                const Media m = (Media)i;
                const CaseDims d = case_dims(m);
                for (float insp : {0.f, 1.f}) {
                    const CentreAdj a = centre_adj(insp > 0.f ? 0.f : 3.14159f, insp);
                    const float rest = display_scale(m) * a.scale_mul, pitch = 0.55f * insp, lift = 0.05f * insp + 0.02f;
                    const float mul = fit_centre_scale(d, rest, pitch, lift, a.z, fs, top_lim, bot_lim);
                    CHECK(mul >= 0.35f && mul <= 1.f);
                    for (int yi = 0; yi < 24; ++yi) for (int pi_ = -2; pi_ <= 2; ++pi_) {
                        float t, b;
                        case_y_extent(d, rest * mul, 3.14159f * (float)yi / 24.f, pitch * (float)pi_ * 0.5f, lift, a.z, fs, &t, &b);
                        if (!(t >= top_lim - 1.f && b <= bot_lim + 1.f)) std::printf("U=%g m=%d insp=%g mul=%g yi=%d pi=%d t=%g b=%g lim %g %g\n", U, i, insp, mul, yi, pi_, t, b, top_lim, bot_lim);
                        CHECK(t >= top_lim - 1.f && b <= bot_lim + 1.f);
                    }
                }
            }
            // a case that already fits is left alone
            CHECK(fit_centre_scale(case_dims(Media::GcCase), 0.5f, 0.f, 0.f, 0.f, fs, f0 - 3.f * U, f0 + 2.f * U) == 1.f);
        }
        // a tall case near the top is shrunk, and shrinking only ever helps
        FitScene fs2; fs2.U = 400.f; fs2.hy = 500.f - 0.55f * 400.f;
        CHECK(fit_centre_scale(case_dims(Media::SwitchCase), 1.3f, 0.f, 0.f, -1.1f, fs2, 500.f - 1.0f * 400.f, 560.f) < 1.f);
    }

    /* cases-faces */
    {
        using shelf::Media;
        // the cover window sits inside the face, the hinge strip is to its left on plastic cases
        for (int i = 0; i < shelf::kMediaCount; ++i) {
            const Media m = (Media)i;
            const UvRect w = front_window(kind_for(m));
            CHECK(w.u0 >= 0.f && w.v0 >= 0.f && w.u1 <= 1.f && w.v1 <= 1.f && w.u1 - w.u0 > 0.85f && w.v1 - w.v0 > 0.9f);
            if (!is_box(m)) CHECK(w.u0 > hinge_frac(kind_for(m)));
            CHECK(nominal_cover_aspect(m) > 0.5f && nominal_cover_aspect(m) < 1.6f);
        }
        // the case shapes follow the real cover scans (libretro Named_Boxarts measure: GC / PS2 / Wii U 0.71, DS / 3DS 1.12, N64 / SNES 1.42, GBA / GBC / GB / PS1 1.0, NES 0.70):
        // none of them is letterboxed, so the scan fills the case's cover window
        struct { Media m; float a; } scans[] = {{Media::GcCase, 0.71f}, {Media::Ps2Case, 0.71f}, {Media::WiiUCase, 0.71f}, {Media::DsCase, 1.12f}, {Media::N3dsCase, 1.13f}, {Media::N64Cart, 1.42f},
                                                {Media::SnesCart, 1.42f}, {Media::GbaCart, 1.0f}, {Media::GbcCart, 1.0f}, {Media::GbCart, 1.0f}, {Media::Ps1Jewel, 1.02f}, {Media::NesCart, 0.70f}};
        for (const auto& s : scans) {
            const CaseArtFit f = fit_art(nominal_cover_aspect(s.m), s.a, nominal_cover_aspect(s.m));
            CHECK(!f.letterbox);
            const float na = nominal_cover_aspect(s.m);
            CHECK(std::fabs(na / s.a - 1.f) < 0.08f);   // within the "exact" stretch (or the window shrinks a little)
        }
        // fit_art: a scan is never cropped nor stretched beyond 7%; the rect stays inside the window and centred
        for (float win : {0.6f, 0.9f, 1.4f}) for (float art : {0.5f, 0.71f, 1.0f, 1.12f, 1.4f, 2.0f}) {
            const CaseArtFit f = fit_art(win, art, win);
            CHECK(f.dst.u0 >= 0.f && f.dst.v0 >= 0.f && f.dst.u1 <= 1.f + 1e-5f && f.dst.v1 <= 1.f + 1e-5f && f.dst.u1 > f.dst.u0 && f.dst.v1 > f.dst.v0);
            CHECK(std::fabs((f.dst.u0 + f.dst.u1) - 1.f) < 1e-4f && std::fabs((f.dst.v0 + f.dst.v1) - 1.f) < 1e-4f);
            const float got = ((f.dst.u1 - f.dst.u0) * win) / (f.dst.v1 - f.dst.v0);
            CHECK(std::fabs(got / art - 1.f) < 0.07f + 1e-4f);
            CHECK(f.exact == (std::fabs(art / win - 1.f) < 0.07f && f.dst.u0 == 0.f && f.dst.v0 == 0.f));
        }
        {   // a wide scan in a tall window: bars above and below, none at the sides; a tall one the other way round
            const CaseArtFit w = fit_art(0.7f, 1.4f, 0.7f), t = fit_art(1.4f, 0.7f, 1.4f);
            CHECK(w.dst.u0 == 0.f && w.dst.u1 == 1.f && w.dst.v0 > 0.2f && !w.exact);
            CHECK(t.dst.v0 == 0.f && t.dst.v1 == 1.f && t.dst.u0 > 0.2f && !t.exact);
            CHECK(w.letterbox && t.letterbox);   // far from the console's nominal aspect: painted with the insert colour
        }
        CHECK(!fit_art(0.7f, 0.7f * 1.14f, 0.7f).letterbox && fit_art(0.7f, 0.7f * 1.2f, 0.7f).letterbox && fit_art(0.7f, 0.7f * 0.86f, 0.7f).letterbox == false && fit_art(0.7f, 0.7f * 0.8f, 0.7f).letterbox);
        CHECK(fit_art(0.f, 1.f, 1.f).exact == false && fit_art(1.f, 0.f, 1.f).dst.u1 == 1.f);   // degenerate input: the whole window, no NaN
        // square art (a Switch game's icon) on a portrait case is composed; DS / N64 style art never is
        CHECK(is_square_art(256, 256) && is_square_art(512, 480) && is_square_art(1000, 1100) && !is_square_art(256, 300) && !is_square_art(0, 0) && !is_square_art(640, 480));
        CHECK(is_portrait_case(Media::SwitchCase) && is_portrait_case(Media::GcCase) && is_portrait_case(Media::PspCase) && !is_portrait_case(Media::DsCase) && !is_portrait_case(Media::N64Cart) && !is_portrait_case(Media::Ps1Jewel));
        // a full cover (back | spine | front): three contiguous uv ranges of one texture, the spine in the middle
        {
            const FullParts p = full_cover_parts(2087, 1008, 1079);
            CHECK(p.back.u0 == 0.f && p.front.u1 == 1.f && p.back.u1 == p.spine.u0 && p.spine.u1 == p.front.u0);
            CHECK(std::fabs(p.back.u1 - 1008.f / 2087.f) < 1e-6f && std::fabs(p.front.u0 - 1079.f / 2087.f) < 1e-6f);
            CHECK(p.spine.u1 - p.spine.u0 > 0.02f && p.spine.u1 - p.spine.u0 < 0.14f);
            CHECK((p.back.u1 - p.back.u0) == (p.front.u1 - p.front.u0) || std::fabs((p.back.u1 - p.back.u0) - (p.front.u1 - p.front.u0)) < 1e-3f);   // equal-width back and front
            const FullParts z = full_cover_parts(0, 0, 0);   // no texture: no NaN
            CHECK(z.back.u1 == 0.f && z.spine.u1 == 0.f);
        }
    }

    /* ---- Coverflow ---- */
    {
        namespace cf = hubui::cases::cf;
        using shelf::Media;
        const float deg = 3.14159265f / 180.f;
        // the setting: Coverflow is the default, Classic persists
        Settings s0;
        CHECK(!s0.lib_cases_classic);
        s0.lib_cases_classic = true;
        Settings s1;
        CHECK(settings_from_json(settings_to_json(s0), &s1) && s1.lib_cases_classic);
        Settings s2;
        CHECK(settings_from_json("{\"lib_layout\": \"cases\"}", &s2) && !s2.lib_cases_classic);   // an older file: Coverflow
        // the selected case: left of the middle, raised, toward the camera (z < 0), turned 22 degrees one way, pitched -4
        const cf::Pose h = cf::slot(0.f);
        CHECK(std::fabs(h.x + 0.17f) < 1e-5f && h.z < -0.39f && h.z > -0.41f && h.lift > 0.f && h.scale == 1.f && h.alpha == 1.f);
        CHECK(std::fabs(h.yaw + 22.f * deg) < 1e-4f && std::fabs(h.pitch + 4.f * deg) < 1e-4f);
        // neighbours a = 1..6: x = +-(1.006 + (a-1) 0.483), z = 0.356 a + 0.263 (behind), yaw +-50 degrees, scale 0.92, a bit lower than the selected case
        for (int a = 1; a <= 6; ++a) {
            const cf::Pose r = cf::slot((float)a), l = cf::slot((float)-a);
            CHECK(std::fabs(r.x - (1.006f + (float)(a - 1) * 0.483f)) < 1e-4f && std::fabs(l.x + r.x) < 1e-5f);
            CHECK(std::fabs(r.z - (0.356f * (float)a + 0.263f)) < 1e-4f && std::fabs(l.z - r.z) < 1e-6f);
            CHECK(std::fabs(r.yaw - 50.f * deg) < 1e-4f && std::fabs(l.yaw + 50.f * deg) < 1e-4f);
            CHECK(std::fabs(r.scale - 0.92f) < 1e-6f && r.lift < h.lift && r.alpha == 1.f && r.pitch == 0.f);
        }
        // fade out beyond 6 instead of popping; opaque up to 6
        CHECK(cf::slot(6.f).alpha == 1.f && cf::slot(6.5f).alpha < 1.f && cf::slot(6.5f).alpha > 0.f && cf::slot(7.3f).alpha == 0.f && cf::slot(-9.f).alpha == 0.f);
        // smooth between slots (no jumps), and the selected case unwinds its 22 degrees while it slides away
        for (float d = -7.f; d < 7.f; d += 0.01f) {
            const cf::Pose p = cf::slot(d), q = cf::slot(d + 0.01f);
            CHECK(std::fabs(q.x - p.x) < 0.02f && std::fabs(q.yaw - p.yaw) < 0.02f && std::fabs(q.z - p.z) < 0.012f);
        }
        CHECK(cf::slot(0.5f).yaw > cf::slot(0.f).yaw && cf::slot(0.5f).yaw < cf::slot(1.f).yaw && cf::slot(0.5f).x > cf::slot(0.f).x && cf::slot(0.5f).x < cf::slot(1.f).x);
        // the display scale: every case about as tall as the stage allows, never wider than 300/323
        for (int i = 0; i < shelf::kMediaCount; ++i) {
            const CaseDims d = case_dims((Media)i);
            const float ds = cf::display_scale((Media)i);
            CHECK(ds >= 0.9f && ds <= 1.5f + 1e-5f && d.h * ds <= 1.0001f && d.w * ds <= 0.9300f);
        }
        CHECK(cf::display_scale(Media::GcCase) == 1.f && cf::display_scale(Media::DsCase) > 1.f && cf::display_scale(Media::SnesCart) < 1.f + 1e-4f);
        // wide / small boxes: the first neighbour never overlaps the selected case, on either side, for every pair of shapes
        {
            int bad = 0;
            for (int i = 0; i < shelf::kMediaCount; ++i)
                for (int j = 0; j < shelf::kMediaCount; ++j) {
                    const CaseDims c = case_dims((Media)i), n = case_dims((Media)j);
                    const float cs = cf::display_scale((Media)i), ns = cf::display_scale((Media)j);
                    float hl, hr, nl, nr;
                    cf::screen_extent(c, cs, cf::kHeroYaw, cf::kHeroX, cf::kHeroZ, &hl, &hr);
                    const float er = cf::neighbour_extra(c, cs, n, ns, true), el = cf::neighbour_extra(c, cs, n, ns, false);
                    CHECK(er >= 0.f && el >= 0.f);
                    cf::screen_extent(n, cf::kNbScale * ns, cf::kNbYaw, cf::nb_x(1) + er, cf::nb_z(1), &nl, &nr);
                    if (nl < hr - 1e-3f) { if (++bad <= 3) std::printf("right overlap %d %d: %.3f\n", i, j, hr - nl); }
                    cf::screen_extent(n, cf::kNbScale * ns, -cf::kNbYaw, -(cf::nb_x(1) + el), cf::nb_z(1), &nl, &nr);
                    if (nr > hl + 1e-3f) { if (++bad <= 3) std::printf("left overlap %d %d: %.3f\n", i, j, nr - hl); }
                }
            CHECK(bad == 0);
            CHECK(cf::neighbour_extra(case_dims(Media::GcCase), 1.f, case_dims(Media::GcCase), 1.f, true) < 0.08f);   // keep cases sit at their slot
        }
        // idle: starts at rest, 6 s loop: bob 0.015, yaw 22 -> 26 degrees, pitch -4 -> -3
        {
            const cf::Idle i0 = cf::idle(0.0), i3 = cf::idle(3.0), i6 = cf::idle(6.0);
            CHECK(std::fabs(i0.lift) < 1e-6f && std::fabs(i0.yaw) < 1e-6f && std::fabs(i0.pitch) < 1e-6f);
            CHECK(std::fabs(i3.lift - 0.015f) < 1e-5f && std::fabs(h.yaw + i3.yaw + 26.f * deg) < 1e-4f && std::fabs(h.pitch + i3.pitch + 3.f * deg) < 1e-4f);
            CHECK(std::fabs(i6.lift) < 1e-5f && std::fabs(cf::idle(9.0).lift - 0.015f) < 1e-5f);
            for (double t = 0; t < 12; t += 0.05) CHECK(cf::idle(t).lift >= -1e-6f && cf::idle(t).lift <= 0.015f + 1e-6f);
        }
        // the glide: cubic-bezier(.22,1,.36,1): 0 -> 1, monotonic, no overshoot, fast start with a long soft settle
        {
            CHECK(cf::ease(0.f) == 0.f && cf::ease(1.f) == 1.f);
            float prev = 0.f;
            for (float t = 0.f; t <= 1.f; t += 0.005f) { const float v = cf::ease(t); CHECK(v >= prev - 1e-6f && v <= 1.f + 1e-6f); prev = v; }
            CHECK(cf::ease(0.1f) > 0.35f && cf::ease(0.5f) > 0.9f && cf::ease(0.5f) < 0.995f);   // mostly there by half time, still settling
            float x = 0.f; cf::EaseState es;
            float t = 0.f; int steps = 0; bool moving = true;
            while (moving && steps < 1000) { moving = cf::ease_step(x, es, 5.f, 1.f / 60.f, false); t += 1.f / 60.f; ++steps; CHECK(x <= 5.f + 1e-5f && x >= 0.f); }
            CHECK(!moving && x == 5.f && std::fabs(t - 0.7f) < 0.05f);   // 0.7 s
            // a new target mid-way restarts from where it is (no jump)
            x = 0.f; es = cf::EaseState();
            for (int i = 0; i < 12; ++i) cf::ease_step(x, es, 3.f, 1.f / 60.f, false);
            const float mid = x;
            cf::ease_step(x, es, 4.f, 1.f / 60.f, false);
            CHECK(x >= mid && x - mid < 0.5f);
            // an outside jump (the scrubber) is followed, not fought
            x = 20.f;
            cf::ease_step(x, es, 21.f, 1.f / 60.f, false);
            CHECK(x >= 20.f && x < 21.f);
            // reduced motion snaps
            cf::ease_step(x, es, 7.f, 1.f / 60.f, true);
            CHECK(x == 7.f);
        }
    }

    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("hub_cases_test: all ok\n");
    return 0;
}
