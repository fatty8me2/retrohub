// Console test for the "was the capture black?" check (ext_media_logic.h) with synthetic BGRA buffers. No windows, no D3D.
#include "ext_media_logic.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using extmedia::all_black;

/* A buffer sized exactly like a captured frame: (h-1)*stride + w*4 bytes, so any over-read is the tail of the vector. */
struct Img {
    int w, h, stride;
    std::vector<uint8_t> px;
    Img(int w_, int h_, int stride_) : w(w_), h(h_), stride(stride_), px(h_ > 0 && w_ > 0 ? size_t(h_ - 1) * stride_ + size_t(w_) * 4 : 0, 0) {}
    Img(int w_, int h_) : Img(w_, h_, w_ * 4) {}
    void set(int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
        uint8_t* p = &px[size_t(y) * stride + size_t(x) * 4];
        p[0] = b; p[1] = g; p[2] = r; p[3] = a;
    }
    void fill(int x0, int y0, int x1, int y1, uint8_t b, uint8_t g, uint8_t r) { for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) set(x, y, b, g, r); }
    bool black() const { return all_black(px.data(), w, h, stride); }
};

int main() {
    {   // pure black, alpha ignored
        Img i(640, 360);
        CHECK(i.black());
        i.fill(0, 0, i.w, i.h, 0, 0, 0);   // opaque black
        CHECK(i.black());
        Img a(640, 360);
        for (int y = 0; y < a.h; ++y) for (int x = 0; x < a.w; ++x) a.px[size_t(y) * a.stride + x * 4 + 3] = 255;
        CHECK(a.black());
    }
    {   // faint noise (1..8 on every channel, every pixel) is black; uniform 8 is black; uniform 10 is picture
        Img i(640, 360);
        std::srand(7);
        for (int y = 0; y < i.h; ++y) for (int x = 0; x < i.w; ++x) i.set(x, y, uint8_t(1 + std::rand() % 8), uint8_t(1 + std::rand() % 8), uint8_t(1 + std::rand() % 8));
        CHECK(i.black());
        Img u8(640, 360); u8.fill(0, 0, u8.w, u8.h, 8, 8, 8); CHECK(u8.black());
        Img u10(640, 360); u10.fill(0, 0, u10.w, u10.h, 10, 10, 10); CHECK(!u10.black());
        Img u9(640, 360); u9.fill(0, 0, u9.w, u9.h, 0, 9, 0); CHECK(!u9.black());
    }
    {   // a single pixel at 9 anywhere (any channel, first/last/off any grid) in a large black frame is picture
        Img a(1280, 720); a.set(1, 1, 9, 0, 0); CHECK(!a.black());
        Img b(1280, 720); b.set(1279, 719, 0, 0, 9); CHECK(!b.black());
        Img c(1280, 720); c.set(641, 357, 0, 9, 0); CHECK(!c.black());
        Img d(1280, 720); d.set(641, 357, 8, 8, 8); CHECK(d.black());
        Img e(1280, 720); e.set(3, 3, 255, 255, 255); CHECK(!e.black());   // a lone hot pixel is picture too
    }
    {   // one thin bright line (1 px) off any coarse grid, horizontal and vertical, in HD and 4K frames: kept
        for (int dims : {0, 1}) {
            const int W = dims ? 3840 : 1920, H = dims ? 2160 : 1080;
            Img h1(W, H); h1.fill(0, 541, W, 542, 255, 255, 255); CHECK(!h1.black());
            Img h2(W, H); h2.fill(0, 4, W, 5, 0, 200, 0); CHECK(!h2.black());
            Img v1(W, H); v1.fill(1001, 0, 1002, H, 255, 255, 255); CHECK(!v1.black());
            Img v2(W, H); v2.fill(W - 1, 0, W, H, 0, 0, 90); CHECK(!v2.black());
            Img z(W, H); CHECK(z.black());
        }
    }
    {   // a dark game scene (mostly 20-40) is picture, in every channel position
        Img i(640, 480);
        std::srand(3);
        for (int y = 0; y < i.h; ++y) for (int x = 0; x < i.w; ++x) i.set(x, y, uint8_t(20 + std::rand() % 21), uint8_t(20 + std::rand() % 21), uint8_t(20 + std::rand() % 21));
        CHECK(!i.black());
        Img j(640, 480); j.fill(0, 0, j.w, j.h, 0, 0, 20); CHECK(!j.black());
        Img k(640, 480); k.fill(0, 0, k.w, k.h, 20, 0, 0); CHECK(!k.black());
    }
    {   // letterboxed picture: black bars, a picture in the middle (also a small one)
        Img i(1920, 1080);
        i.fill(0, 140, 1920, 940, 60, 90, 120);
        CHECK(!i.black());
        Img j(1920, 1080);
        j.fill(900, 500, 1020, 580, 120, 120, 120);
        CHECK(!j.black());
    }
    {   // tiny frames: a single lit pixel is picture (too little data to call it black)
        Img one(1, 1); CHECK(one.black());
        one.set(0, 0, 0, 90, 0); CHECK(!one.black());
        Img row(300, 1); row.set(295, 0, 0, 0, 70); CHECK(!row.black());
        Img col(1, 300); col.set(0, 294, 0, 70, 0); CHECK(!col.black());
        Img small(64, 48); CHECK(small.black());
        small.set(1, 1, 255, 255, 255); CHECK(!small.black());
    }
    {   // zero / negative sizes or no buffer: nothing read, reported black (there is no picture)
        CHECK(all_black(nullptr, 0, 0, 0));
        CHECK(all_black(nullptr, 0, 10, 0));
        CHECK(all_black(nullptr, 10, 0, 40));
        CHECK(all_black(nullptr, -5, -5, 0));
        uint8_t px[4] = {255, 255, 255, 255};
        CHECK(all_black(px, 0, 1, 4));
        CHECK(all_black(px, 1, -1, 4));
    }
    {   // stride wider than the row, including odd (non multiple of 4) strides: padding bytes are never read as picture
        for (int pad : {1, 2, 3, 4, 7, 61}) {
            Img i(133, 129, 133 * 4 + pad);
            for (int y = 0; y < i.h - 1; ++y) for (int p = 0; p < pad; ++p) i.px[size_t(y) * i.stride + 133 * 4 + p] = 0xFF;   // padding is bright
            CHECK(i.black());
            i.fill(0, 64, i.w, 65, 0, 0, 200);   // a full bright row
            CHECK(!i.black());
        }
        // exact-size buffers: the last row / column must not read past the end (a sanitizer or guard page would catch it)
        Img e(11, 15); CHECK(e.black());
        Img e2(6, 8, 6 * 4 + 3); CHECK(e2.black());
        e2.set(5, 7, 0, 0, 99); CHECK(!e2.black());
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hub_ext_media_logic_test: all passed\n");
    return 0;
}
