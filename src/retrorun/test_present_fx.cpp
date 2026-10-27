// Console test + benchmark for retrorun's screen filters and bezels (present_fx.h). Exit code 0 = all passed.
#include "present_fx.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    } while (0)

int main() {
    using namespace fx;
    Filter f;
    CHECK(parse_filter("CRT", &f) && f == Filter::Crt);
    CHECK(parse_filter("none", &f) && f == Filter::Off);
    CHECK(!parse_filter("bogus", &f));
    Filter c = Filter::Off;
    for (int i = 0; i < 5; i++) c = next_filter(c);
    CHECK(c == Filter::Off);

    // GBA-like 240x160 test pattern into a 1440x960 integer-scaled rect.
    const int sw = 240, sh = 160, dw = 1440, dh = 960;
    std::vector<uint32_t> src((size_t)sw * sh);
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) src[(size_t)y * sw + x] = 0xFF000000u | ((x * 255 / sw) << 16) | ((y * 255 / sh) << 8) | (((x ^ y) & 1) ? 200 : 40);
    Src s{src.data(), sw, sw, sh};

    Engine e;
    e.set_threads(1);
    for (Filter fl : {Filter::Crt, Filter::Lcd, Filter::Dmg, Filter::Sharp}) {
        std::vector<uint32_t> out;
        e.render(fl, s, dw, dh, &out);
        CHECK(out.size() == (size_t)dw * dh);
        bool opaque = true;
        for (size_t i = 0; i < out.size(); i += 997) if ((out[i] >> 24) != 255) opaque = false;
        CHECK(opaque);
        if (fl == Filter::Sharp) {   // the middle of a source pixel keeps its exact colour at an integer scale
            int x = 100, y = 70;
            CHECK(out[(size_t)(y * 6 + 3) * dw + x * 6 + 3] == src[(size_t)y * sw + x]);
        }
        if (fl == Filter::Dmg) {
            uint32_t p = out[(size_t)483 * dw + 723];
            CHECK(((p >> 8) & 255) > ((p >> 16) & 255) && ((p >> 8) & 255) > (p & 255));
        }
        if (fl == Filter::Crt) {   // scanlines: a row between source rows is darker than a row at a source-row centre
            auto sum = [&](int y) { unsigned t = 0; for (int x = 600; x < 840; x++) t += (out[(size_t)y * dw + x] >> 8) & 255; return t; };
            CHECK(sum(480 + 3) > sum(480 + 6 - 1) || sum(483) > sum(485));
        }
        auto t0 = std::chrono::steady_clock::now();
        const int N = 30;
        for (int i = 0; i < N; i++) e.render(fl, s, dw, dh, &out);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("%-6s %dx%d -> %dx%d: %.2f ms/frame (1 thread)\n", filter_name(fl), sw, sh, dw, dh, ms);
        CHECK(ms < 12.0);   // generous: slow CI boxes; the real target (<2 ms at 1080p) is checked by eye in the log
    }
    {   // multi-threaded path agrees with the single-threaded one
        std::vector<uint32_t> a, b;
        Engine e1, e4; e1.set_threads(1); e4.set_threads(4);
        e1.render(Filter::Crt, s, dw, dh, &a); e4.render(Filter::Crt, s, dw, dh, &b);
        CHECK(a == b);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; i++) e4.render(Filter::Crt, s, dw, dh, &b);
        std::printf("crt    4 threads: %.2f ms/frame\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30);
    }
    {   // odd, non-integer sizes must not crash or read out of range
        std::vector<uint32_t> out;
        for (Filter fl : {Filter::Crt, Filter::Lcd, Filter::Dmg, Filter::Sharp}) {
            e.render(fl, s, 333, 217, &out);  CHECK(out.size() == 333u * 217u);
            e.render(fl, s, 100, 60, &out);   CHECK(out.size() == 6000u);
            e.render(fl, Src{src.data(), sw, 1, 1}, 50, 50, &out); CHECK(out.size() == 2500u);
        }
    }
    {   // bezels
        for (const char* sys : {"gb", "gbc", "gba", "nes", "snes", "nds", "ps1", "other"}) {
            Engine be;
            Rect r{240, 60, 1440, 960};
            const auto& bz = be.bezel(sys, 1920, 1080, r);
            CHECK(bz.size() == 1920u * 1080u);
            CHECK(bz[(size_t)540 * 1920 + 960] == 0xFF000000u);           // screen area is black
            CHECK(bz[(size_t)540 * 1920 + 20] != 0xFF000000u || std::string(sys) == "other");   // bars are decorated
            CHECK(&be.bezel(sys, 1920, 1080, r) == &bz);                  // cached
        }
        Engine be; be.bezel("gba", 64, 48, Rect{0, 0, 64, 48});          // no bars: must not crash
    }
    std::printf(g_fail ? "present_fx: %d FAILED\n" : "present_fx: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
