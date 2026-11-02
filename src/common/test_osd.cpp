#include <algorithm>
#include <cstdio>
#include <string>

#include "osd.h"
#include "perf_overlay.h"
#include "../net/username.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static uint64_t g_t = 1000;
static uint64_t clk() { return g_t; }

static int count_lit(const osd::Layer& L) { int n = 0; for (uint32_t p : L.px) if (p >> 24) n++; return n; }

int main() {
    CHECK(hubnet::username_ascii("Zo\xC3\xAB \xC3\x91" "and\xC3\xBA \xC3\x86\xC3\x9F" "\xE4\xB8\xAD") == "Zoe Nandu Aess");
    osd::set_clock_for_test(clk);
    osd::Layer L;
    bool ch = false;
    CHECK(!osd::render(&L, 320, 240, &ch));          // nothing yet
    osd::push("Saved state slot0", osd::Kind::Good, 2.0);
    g_t += 500;                                       // past the fade-in
    CHECK(osd::render(&L, 320, 240, &ch) && ch && L.w == 320 && L.h == 240 && count_lit(L) > 200);
    CHECK(osd::latest_text() == "Saved state slot0");
    CHECK(osd::render(&L, 320, 240, &ch) && !ch);     // unchanged pixels
    osd::push("Screenshot saved");
    CHECK(osd::render(&L, 320, 240, &ch) && ch);
    bool top_right = false;
    for (int y = 0; y < 60; y++) for (int x = 200; x < 320; x++) if (L.px[(size_t)y * 320 + x] >> 24) top_right = true;
    CHECK(!top_right);
    osd::set_corner("REC", osd::Kind::Error);
    CHECK(osd::render(&L, 320, 240, &ch));
    for (int y = 0; y < 60; y++) for (int x = 200; x < 320; x++) if (L.px[(size_t)y * 320 + x] >> 24) top_right = true;
    CHECK(top_right);
    g_t += 5000;                                      // toasts expire, corner stays
    CHECK(osd::render(&L, 320, 240, &ch) && osd::latest_text().empty());
    osd::set_corner("");
    CHECK(!osd::render(&L, 320, 240, &ch));
    osd::push("Speed 2x"); osd::push("Speed 2x");     // duplicate text refreshes instead of stacking
    g_t += 500;
    osd::render(&L, 320, 240, &ch);
    int one = count_lit(L);
    osd::clear();
    osd::push("Speed 2x");
    g_t += 500;
    osd::render(&L, 320, 240, &ch);
    CHECK(count_lit(L) == one);
    std::vector<uint32_t> fb(320 * 240, 0xFF000000u);
    osd::blend(fb.data(), 320, 240, 320, L);
    int changed = 0; for (uint32_t p : fb) if (p != 0xFF000000u) changed++;
    CHECK(changed > 50);
    CHECK(osd::text_width("ab", 2) == 22 && osd::scale_for_height(1080) == 4 && osd::scale_for_height(100) == 1);
    osd::push(std::string("bad\x01\xff", 5));        // non-ASCII must not crash
    osd::render(&L, 64, 48, &ch);
    {   // stats slot (top-left) + perf overlay text
        osd::clear();
        osd::set_stats("60 FPS");
        CHECK(osd::render(&L, 320, 240, &ch));
        bool tl = false;
        for (int y = 0; y < 30; y++) for (int x = 0; x < 100; x++) if (L.px[(size_t)y * 320 + x] >> 24) tl = true;
        CHECK(tl);
        osd::set_stats("");
        CHECK(!osd::render(&L, 320, 240, &ch));
        perfov::Sample s; s.fps = 59.6;
        CHECK(perfov::format(s) == "60 FPS");
        s.rtt_ms = 31.6; s.delay = 3; s.resim_per_s = 12.2;
        CHECK(perfov::format(s) == "60 FPS | PING 32 ms | DELAY 3 | ROLLBACK 12/s");
        perfov::FpsMeter m;
        bool upd = false;
        for (uint64_t t = 0; t <= 1000; t += 16) upd |= m.frame(t);
        CHECK(upd && m.fps() > 60 && m.fps() < 64);
        perfov::RateMeter r;
        r.update(0, 0);
        CHECK(r.update(500, 10) == 20.0);
    }
    // ---- corner texts (set_corner_at, the runner_ctl "osd_corner" command)
    {
        struct Box { int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; bool any() const { return x1 >= 0; } };
        auto bbox = [](const osd::Layer& l) {
            Box b;
            for (int y = 0; y < l.h; y++)
                for (int x = 0; x < l.w; x++)
                    if (l.px[(size_t)y * l.w + x] >> 24) { b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y); b.x1 = std::max(b.x1, x); b.y1 = std::max(b.y1, y); }
            return b;
        };
        auto shot = [&](int w, int h) { osd::Layer l; bool c = false; osd::render(&l, w, h, &c); return l; };
        const int W = 320, H = 240;
        osd::clear();
        g_t += 10000;
        CHECK(!osd::render(&L, W, H, &ch));
        // each corner lands in its own quadrant of the layer
        for (int c = 0; c < 4; c++) {
            osd::clear();
            osd::set_corner_at(c, "ROUND 2", osd::Kind::Good);
            CHECK(osd::render(&L, W, H, &ch) && ch);
            Box b = bbox(L);
            CHECK(b.any());
            CHECK((c == 0 || c == 2) ? b.x1 < W / 2 : b.x0 > W / 2);   // left corners in the left half, right corners in the right half
            CHECK((c == 0 || c == 1) ? b.y1 < H / 2 : b.y0 > H / 2);   // top corners in the top half, bottom corners in the bottom half
        }
        // "" clears a corner; an out-of-range corner is ignored
        osd::clear();
        osd::set_corner_at(3, "LAP 2");
        CHECK(osd::render(&L, W, H, &ch));
        osd::set_corner_at(3, "");
        CHECK(!osd::render(&L, W, H, &ch));
        osd::set_corner_at(-1, "x"); osd::set_corner_at(4, "x"); osd::set_corner_at(99, "x");
        CHECK(!osd::render(&L, W, H, &ch));
        // set_corner is the top-right slot: the same slot as set_corner_at(1, ...)
        osd::set_corner("REC 00:12", osd::Kind::Error);
        CHECK(osd::render(&L, W, H, &ch));
        Box rec = bbox(L);
        osd::set_corner_at(1, "REC 00:12", osd::Kind::Error);
        osd::Layer same = shot(W, H);
        CHECK(same.px == L.px);
        osd::set_corner_at(1, "STANDINGS", osd::Kind::Good);   // replaces the REC text instead of drawing next to it
        osd::Layer rep = shot(W, H);
        Box rb = bbox(rep);
        CHECK(rb.x1 == rec.x1 && rb.y0 == rec.y0);              // right-aligned at the same margin
        osd::set_corner("");
        CHECK(!osd::render(&L, W, H, &ch));
        // top-left: under the stats line, never on top of it
        osd::clear();
        osd::set_stats("60 FPS");
        osd::Layer st_only = shot(W, H);
        Box sb = bbox(st_only);
        osd::set_corner_at(0, "P1 3  P2 1");
        osd::Layer both = shot(W, H);
        Box bb = bbox(both);
        CHECK(sb.any() && bb.y1 > sb.y1 && bb.y0 == sb.y0);      // the union grew downwards only
        bool stats_untouched = true;
        for (int y = 0; y <= sb.y1; y++) for (int x = 0; x < W; x++) if (both.px[(size_t)y * W + x] != st_only.px[(size_t)y * W + x]) stats_untouched = false;
        CHECK(stats_untouched);
        osd::set_stats("");
        osd::Layer c0_only = shot(W, H);
        CHECK(bbox(c0_only).y0 == sb.y0);                        // without a stats line the text moves up to the top row
        // bottom-left: above the room the four toasts can take
        osd::clear();
        for (int i = 0; i < 6; i++) osd::push("Toast number " + std::to_string(i));   // only the newest four show
        g_t += 500;
        osd::Layer toasts = shot(W, H);
        Box tb = bbox(toasts);
        osd::set_corner_at(2, "ROUND 3 OF 5");
        osd::Layer with_c2 = shot(W, H);
        osd::push("Toast number 6");                              // a fifth toast scrolls in: nothing may collide
        g_t += 500;
        osd::Layer with_5 = shot(W, H);
        int lit_c2 = 0, overlap = 0;
        for (size_t i = 0; i < with_c2.px.size(); i++) {
            const bool a = (with_c2.px[i] >> 24) != 0, t = (toasts.px[i] >> 24) != 0;
            if (a && !t) lit_c2++;
            if (a && t && with_c2.px[i] != toasts.px[i]) overlap++;
        }
        CHECK(lit_c2 > 100 && overlap == 0);
        int topmost_new = H;
        for (int y = 0; y < H && topmost_new == H; y++) for (int x = 0; x < W; x++) if (with_c2.px[(size_t)y * W + x] != toasts.px[(size_t)y * W + x]) { topmost_new = y; break; }
        CHECK(tb.any() && topmost_new < tb.y0);                  // the corner text starts above the whole toast stack...
        int bottom_new = -1;
        for (int y = H - 1; y >= 0 && bottom_new < 0; y--) for (int x = 0; x < W; x++) if (with_c2.px[(size_t)y * W + x] != toasts.px[(size_t)y * W + x]) { bottom_new = y; break; }
        CHECK(bottom_new < tb.y0);                               // ...and ends above it too
        CHECK(with_5.px.size() == with_c2.px.size());
        // bottom-right is free: toasts do not touch it
        osd::set_corner_at(3, "TIME 1:30");
        osd::Layer c3 = shot(W, H);
        int right_half_new = 0;
        for (int y = 0; y < H; y++) for (int x = W / 2; x < W; x++) if (c3.px[(size_t)y * W + x] != with_5.px[(size_t)y * W + x]) right_half_new++;
        CHECK(right_half_new > 100);
        // expiry: seconds > 0 removes the text, 0 keeps it; a repeated call refreshes the timer
        osd::clear();
        osd::set_corner_at(3, "TIME 0:03", osd::Kind::Warn, 3.0);
        osd::set_corner_at(0, "KEEP");
        g_t += 2000;
        CHECK(bbox(shot(W, H)).x1 > W / 2);                      // the timer is still up
        osd::set_corner_at(3, "TIME 0:03", osd::Kind::Warn, 3.0);   // same text again: only the timer restarts
        g_t += 2000;
        CHECK(bbox(shot(W, H)).x1 > W / 2);                      // 4 s after the first call, 2 s after the refresh: still up
        g_t += 1500;                                              // 3.5 s after the refresh: gone
        Box after = bbox(shot(W, H));
        CHECK(after.any() && after.x1 < W / 2);                   // only KEEP (top-left) remains
        osd::set_corner_at(0, "");
        CHECK(!osd::render(&L, W, H, &ch));                       // nothing left: false again
        // seconds <= 0 never expires
        osd::set_corner_at(2, "FOREVER", osd::Kind::Info, 0);
        g_t += 3600 * 1000;
        CHECK(bbox(shot(W, H)).any());
        // several lines (standings), at most four
        osd::clear();
        osd::set_corner_at(1, "1 KAI  3");
        Box one = bbox(shot(W, H));
        osd::set_corner_at(1, "1 KAI  3\n2 MIA  2\n3 BO  1");
        Box three = bbox(shot(W, H));
        osd::set_corner_at(1, "a\nb\nc\nd\ne\nf");
        Box six = bbox(shot(W, H));
        CHECK(three.y1 - three.y0 > 2 * (one.y1 - one.y0) && three.y0 == one.y0);
        osd::set_corner_at(1, "a\nb\nc\nd");
        Box four = bbox(shot(W, H));
        CHECK(six.y1 == four.y1);                                  // lines 5 and 6 are dropped
        osd::set_corner_at(1, "x\n\n  \ny");                        // blank lines are dropped
        osd::set_corner_at(1, "x\ny");
        osd::Layer xy = shot(W, H);
        osd::set_corner_at(1, "x\n\n  \ny");
        CHECK(shot(W, H).px == xy.px);
        // text wider than the layer is cut with "..." and stays inside it; a tiny layer never crashes
        osd::clear();
        osd::set_corner_at(3, std::string(120, 'W'));
        osd::Layer narrow = shot(160, 120);
        Box nb = bbox(narrow);
        CHECK(nb.any() && nb.x0 >= 0 && nb.x1 < 160);
        osd::set_corner_at(0, "tiny");
        osd::Layer tiny = shot(12, 9);
        CHECK(tiny.w == 12);
        // non-ASCII is folded like every OSD text
        osd::clear();
        osd::set_corner_at(0, "Zo\xC3\xAB wins");
        osd::Layer plain_a = shot(W, H);
        osd::set_corner_at(0, "Zoe wins");
        CHECK(shot(W, H).px == plain_a.px);
        // clear() drops every corner
        osd::set_corner_at(0, "a"); osd::set_corner_at(1, "b"); osd::set_corner_at(2, "c"); osd::set_corner_at(3, "d");
        osd::clear();
        CHECK(!osd::render(&L, W, H, &ch));
        // blend still works with corner texts
        osd::set_corner_at(3, "END");
        osd::Layer endl_ = shot(W, H);
        std::vector<uint32_t> fb2((size_t)W * H, 0xFF000000u);
        osd::blend(fb2.data(), W, H, W, endl_);
        int lit2 = 0; for (uint32_t p : fb2) if (p != 0xFF000000u) lit2++;
        CHECK(lit2 > 20);
        osd::clear();
    }
    std::printf(fails ? "osd_test FAILED\n" : "osd_test OK\n");
    return fails ? 1 : 0;
}
