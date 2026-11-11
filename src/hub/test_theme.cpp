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
    CHECK(mid.c[S_WASH] == rgba(96, 99, 108) && mid.c[S_WASH2] == rgba(118, 122, 132) && mid.c[S_NOART0] == rgba(92, 95, 104));
    CHECK(mid.c[S_NOART1] == rgba(70, 73, 82) && mid.c[S_RING] == rgba(6, 7, 12) && mid.c[S_ON_WASH] == rgba(255, 255, 255) && mid.c[S_PILL_FG] == rgba(255, 255, 255));
    for (int p = 0; p < P_COUNT; ++p) {
        Palette q = preset(p);
        for (int s = 0; s < S_COUNT; ++s) CHECK(q.c[s] != 0);                       // every slot defined
        for (int s = 0; s < S_COUNT; ++s) CHECK(ca(q.c[s]) > 0);                    // ... and not fully transparent (a missed slot stays 0)
        for (int s = 0; s < S_COUNT; ++s) if (s != S_HAIRLINE) CHECK(ca(q.c[s]) == 255);
        CHECK(ca(q.c[S_HAIRLINE]) > 0 && ca(q.c[S_HAIRLINE]) < 100);
        double ca1 = contrast(q.c[S_ACCENT], rgba(255, 255, 255)), ca2 = contrast(q.c[S_ACCENT], q.c[S_BG]);
        CHECKC(ca1 >= kMinAccent, "white on ACCENT", p, ca1);
        CHECKC(ca2 >= kMinAccent, "ACCENT on BG", p, ca2);
        const Slot sem[] = {S_GOOD, S_BAD, S_WARN};
        for (Slot sm : sem) { double c = contrast(q.c[sm], q.c[S_PANEL]); CHECKC(c >= 3.0, "semantic on PANEL", p, c); }
        Choice cc; cc.preset = p; cc.cover = true;
        CHECK(parse_choice(preset_id(p)).preset == p);
        CHECK(parse_choice(format_choice(cc)).cover && parse_choice(format_choice(cc)).preset == p);
    }

    // Contrast table: every text token against every surface it is drawn on, every preset. Printed always, enforced per row.
    const uint32_t WHITE = rgba(255, 255, 255);
    const Slot surf[] = {S_BG, S_SIDEBAR, S_PANEL, S_PANEL2, S_PANEL3, S_FRAME};
    const char* surfN[] = {"BG", "SIDEBAR", "PANEL", "PANEL2", "PANEL3", "FRAME"};
    std::printf("contrast table (WCAG ratio; min in brackets)\n");
    for (int p = 0; p < P_COUNT; ++p) {
        Palette q = preset(p);
        std::printf("[%s]\n", preset_id(p));
        auto row = [&](const char* fgN, uint32_t fg, const char* bgN, uint32_t bg, double mn) {
            double r = contrast(fg, bg);
            std::printf("  %-10s on %-8s %5.2f (>=%.1f)%s\n", fgN, bgN, r, mn, r >= mn ? "" : "  <-- FAIL");
            if (r < mn) { std::printf("FAIL preset %s: %s on %s = %.2f, needs %.1f\n", preset_id(p), fgN, bgN, r, mn); ++fails; }
        };
        for (int i = 0; i < 6; ++i) {
            row("TEXT", q.c[S_TEXT], surfN[i], q.c[surf[i]], kMinText);
            row("TEXT2", q.c[S_TEXT2], surfN[i], q.c[surf[i]], kMinText2);
            row("TEXT3", q.c[S_TEXT3], surfN[i], q.c[surf[i]], (surf[i] == S_PANEL2 || surf[i] == S_PANEL3) ? kMinText3Raised : kMinText3);
        }
        row("GHOST", q.c[S_GHOST], "PANEL2", q.c[S_PANEL2], kMinText2);
        row("GHOST", q.c[S_GHOST], "PANEL3", q.c[S_PANEL3], kMinText2);
        row("white", WHITE, "ACCENT", q.c[S_ACCENT], kMinLarge);          // bold button labels
        row("ACCENT", q.c[S_ACCENT], "BG", q.c[S_BG], kMinLarge);
        for (Slot sm : {S_GOOD, S_BAD, S_WARN}) {
            const char* n = sm == S_GOOD ? "GOOD" : sm == S_BAD ? "BAD" : "WARN";
            row(n, q.c[sm], "PANEL", q.c[S_PANEL], kMinLarge);
            row(n, q.c[sm], "PANEL2", q.c[S_PANEL2], kMinLarge);
        }
        row("ON_WASH", q.c[S_ON_WASH], "WASH", q.c[S_WASH], kMinLarge);
        row("PILL_FG", q.c[S_PILL_FG], "PANEL2", q.c[S_PANEL2], kMinText2);
        // disabled controls (BeginDisabled) fade everything to 60% (ImGui DisabledAlpha); 3:1 by design, not 4.5
        for (int i : {0, 2, 3}) row("TEXT@60%", mix_rgb(q.c[surf[i]], q.c[S_TEXT], 0.6), surfN[i], q.c[surf[i]], kMinDisabled);
        row("GHOST@60%", mix_rgb(q.c[S_PANEL2], q.c[S_GHOST], 0.6), "PANEL2", q.c[S_PANEL2], kMinDisabled);
        row("TEXTDIS", q.c[S_TEXT2], "PANEL", q.c[S_PANEL], kMinText2);   // ImGuiCol_TextDisabled is TEXT2 at full alpha
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
