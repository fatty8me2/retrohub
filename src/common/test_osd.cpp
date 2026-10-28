#include <cstdio>

#include "osd.h"
#include "perf_overlay.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static uint64_t g_t = 1000;
static uint64_t clk() { return g_t; }

static int count_lit(const osd::Layer& L) { int n = 0; for (uint32_t p : L.px) if (p >> 24) n++; return n; }

int main() {
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
    std::printf(fails ? "osd_test FAILED\n" : "osd_test OK\n");
    return fails ? 1 : 0;
}
