// Console test: every theme preset defines every palette slot and meets minimum text contrast; cover accent stays readable.
#include "theme.h"
#include <cstdio>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
#define CHECKC(c, msg, p, v) do { if (!(c)) { std::printf("FAIL line %d: %s = %.2f (preset %s)\n", __LINE__, msg, (double)(v), hubtheme::preset_id(p)); ++fails; } } while (0)

int main() {
    using namespace hubtheme;
    Palette mid = preset(P_MIDNIGHT);
    CHECK(mid.c[S_BG] == rgba(16, 17, 22) && mid.c[S_PANEL] == rgba(26, 28, 36) && mid.c[S_ACCENT] == rgba(108, 122, 255));
    CHECK(mid.c[S_TEXT] == rgba(238, 240, 246) && mid.c[S_HAIRLINE] == rgba(255, 255, 255, 20) && mid.c[S_WARN] == rgba(240, 176, 64));
    for (int p = 0; p < P_COUNT; ++p) {
        Palette q = preset(p);
        for (int s = 0; s < S_COUNT; ++s) CHECK(q.c[s] != 0);                       // every slot defined
        for (int s = 0; s < S_COUNT; ++s) if (s != S_HAIRLINE) CHECK(ca(q.c[s]) == 255);
        CHECK(ca(q.c[S_HAIRLINE]) > 0 && ca(q.c[S_HAIRLINE]) < 100);
        const Slot surf[] = {S_BG, S_SIDEBAR, S_PANEL, S_PANEL2};
        for (Slot b : surf) {
            double c1 = contrast(q.c[S_TEXT], q.c[b]), c2 = contrast(q.c[S_TEXT2], q.c[b]);
            CHECKC(c1 >= kMinText, "TEXT contrast", p, c1);
            CHECKC(c2 >= kMinText2, "TEXT2 contrast", p, c2);
        }
        const Slot base[] = {S_BG, S_PANEL};
        for (Slot b : base) {
            double c3 = contrast(q.c[S_TEXT3], q.c[b]);
            CHECKC(c3 >= kMinText3, "TEXT3 contrast", p, c3);
        }
        double ca1 = contrast(q.c[S_ACCENT], rgba(255, 255, 255)), ca2 = contrast(q.c[S_ACCENT], q.c[S_BG]);
        CHECKC(ca1 >= kMinAccent, "white on ACCENT", p, ca1);
        CHECKC(ca2 >= kMinAccent, "ACCENT on BG", p, ca2);
        const Slot sem[] = {S_GOOD, S_BAD, S_WARN};
        for (Slot sm : sem) { double c = contrast(q.c[sm], q.c[S_PANEL]); CHECKC(c >= 3.0, "semantic on PANEL", p, c); }
        Choice cc; cc.preset = p; cc.cover = true;
        CHECK(parse_choice(preset_id(p)).preset == p);
        CHECK(parse_choice(format_choice(cc)).cover && parse_choice(format_choice(cc)).preset == p);
    }
    CHECK(parse_choice("").preset == P_MIDNIGHT && !parse_choice("junk").cover);

    // accent from cover: always readable on every preset, whatever the cover colour
    std::vector<uint32_t> covers = {rgba(255, 255, 0), rgba(250, 250, 90), rgba(10, 10, 140), rgba(255, 0, 0), rgba(0, 255, 255), rgba(120, 60, 20), rgba(60, 60, 255)};
    for (int p = 0; p < P_COUNT; ++p)
        for (uint32_t cv : covers) {
            uint32_t a = readable_accent(cv, preset(p).c[S_BG]);
            CHECKC(contrast(a, preset(p).c[S_BG]) >= kMinAccent, "cover accent on BG", p, contrast(a, preset(p).c[S_BG]));
            CHECKC(contrast(a, rgba(255, 255, 255)) >= kMinAccent, "white on cover accent", p, contrast(a, rgba(255, 255, 255)));
        }
    // average_saturated ignores greys and picks the colourful pixels
    unsigned char img[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) { bool red = i % 2; img[i*4] = red ? 220 : 128; img[i*4+1] = red ? 20 : 128; img[i*4+2] = red ? 30 : 128; img[i*4+3] = 255; }
    uint32_t avg = average_saturated(img, 4, 4, 1);
    CHECK(cr(avg) > 200 && cg(avg) < 40);
    unsigned char grey[16 * 4];
    for (int i = 0; i < 16 * 4; ++i) grey[i] = 128;
    CHECK(average_saturated(grey, 4, 4, 1) == 0 && average_saturated(nullptr, 4, 4) == 0);

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
