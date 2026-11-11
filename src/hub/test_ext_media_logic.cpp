// Console test for the "was the capture black?" check (ext_media_logic.h) with synthetic BGRA buffers. No windows, no D3D.
#include "ext_media_logic.h"
#include <cstdio>
#include <vector>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using extmedia::all_black;

/* A buffer sized exactly like a captured frame: (h-1)*stride + w*4 bytes, so any over-read is the tail of the vector. */
struct Img {
    int w, h, stride;
    std::vector<uint8_t> px;
    Img(int w_, int h_, int stride_) : w(w_), h(h_), stride(stride_), px(h_ > 0 && w_ > 0 ? size_t(h_ - 1) * stride_ + size_t(w_) * 4 : 0, 0) {}
    void set(int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
        uint8_t* p = &px[size_t(y) * stride + size_t(x) * 4];
        p[0] = b; p[1] = g; p[2] = r; p[3] = a;
    }
    bool black() const { return all_black(px.data(), w, h, stride); }
};

int main() {
    {   // all black
        Img i(64, 48, 64 * 4);
        CHECK(i.black());
        // alpha is ignored: fully opaque black is still black
        for (int y = 0; y < i.h; ++y) for (int x = 0; x < i.w; ++x) i.set(x, y, 0, 0, 0, 255);
        CHECK(i.black());
    }
    {   // a bright pixel on the sampled grid (every 5th column, every 7th row) is seen, in each channel
        Img i(64, 48, 64 * 4);
        i.set(0, 0, 255, 255, 255); CHECK(!i.black());
        Img b(64, 48, 64 * 4); b.set(10, 14, 255, 0, 0); CHECK(!b.black());
        Img g(64, 48, 64 * 4); g.set(10, 14, 0, 255, 0); CHECK(!g.black());
        Img r(64, 48, 64 * 4); r.set(10, 14, 0, 0, 255); CHECK(!r.black());
        Img last(64, 48, 64 * 4); last.set(60, 42, 1, 1, 1); CHECK(!last.black());   // last sampled column / row
    }
    {   // near-black noise: ANY non-zero channel on the grid counts as picture (there is no threshold)
        Img i(64, 48, 64 * 4);
        i.set(5, 7, 1, 0, 0); CHECK(!i.black());
        Img j(64, 48, 64 * 4); j.set(5, 7, 0, 0, 2); CHECK(!j.black());
        Img k(64, 48, 64 * 4); k.set(5, 7, 0, 0, 0, 200); CHECK(k.black());   // alpha alone is not picture
    }
    {   // KNOWN LIMIT (by design, sampled): a bright pixel off the grid is not seen. A single lit pixel can be missed.
        Img i(64, 48, 64 * 4);
        i.set(1, 1, 255, 255, 255);
        CHECK(i.black());   // characterisation of current behaviour, not an endorsement
        i.set(4, 6, 255, 255, 255);
        CHECK(i.black());
        // but any real picture (every pixel lit) is never mistaken for black
        for (int y = 0; y < i.h; ++y) for (int x = 0; x < i.w; ++x) i.set(x, y, 40, 40, 40);
        CHECK(!i.black());
    }
    {   // zero / tiny sizes: nothing sampled, no read at all
        CHECK(all_black(nullptr, 0, 0, 0));
        CHECK(all_black(nullptr, 0, 10, 0));
        CHECK(all_black(nullptr, 10, 0, 40));
        CHECK(all_black(nullptr, -5, -5, 0));
        Img one(1, 1, 4); CHECK(one.black());
        one.set(0, 0, 0, 9, 0); CHECK(!one.black());
        Img row(300, 1, 300 * 4); row.set(295, 0, 0, 0, 7); CHECK(!row.black());
        Img col(1, 300, 4); col.set(0, 294, 0, 7, 0); CHECK(!col.black());
    }
    {   // stride wider than the row, including odd (non multiple of 4) strides: padding bytes are never read as picture
        for (int pad : {1, 2, 3, 4, 7, 61}) {
            Img i(33, 29, 33 * 4 + pad);
            for (int y = 0; y < i.h - 1; ++y) for (int p = 0; p < pad; ++p) i.px[size_t(y) * i.stride + 33 * 4 + p] = 0xFF;   // padding is bright
            CHECK(i.black());
            i.set(10, 14, 0, 0, 5);   // row 14 sits at 14*stride
            CHECK(!i.black());
        }
        // exact-size buffer: the last sampled row/col must not read past the end (would crash under a sanitizer / guard page)
        Img e(11, 15, 11 * 4); CHECK(e.black());
        Img e2(6, 8, 6 * 4 + 3); CHECK(e2.black());
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hub_ext_media_logic_test: all passed\n");
    return 0;
}
