#include "game_page_layout.h"

#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    using namespace hubui::gp;

    /* ---- tiers: the sheet's height in spacing units ---- */
    CHECK(tier_for(1080.f, 8.f) == Tier::Full);      // 1080p at 100 %
    CHECK(tier_for(900.f, 8.f) == Tier::Full);
    CHECK(tier_for(760.f, 8.f) == Tier::Full);       // boundary: 95 units
    CHECK(tier_for(759.f, 8.f) == Tier::Compact);
    CHECK(tier_for(720.f, 8.f) == Tier::Compact);    // 1280x720 at 100 %
    CHECK(tier_for(600.f, 8.f) == Tier::Compact);    // 1024x600 at 100 %
    CHECK(tier_for(1080.f, 12.f) == Tier::Compact);  // 1080p at 150 %
    CHECK(tier_for(768.f, 12.f) == Tier::Compact);   // boundary: 64 units
    CHECK(tier_for(767.f, 12.f) == Tier::Tight);
    CHECK(tier_for(720.f, 12.f) == Tier::Tight);     // 720p at 150 %
    CHECK(tier_for(600.f, 12.f) == Tier::Tight);     // 1024x600 at 150 %
    CHECK(tier_for(1080.f, 16.f) == Tier::Compact);  // 1080p at 200 %
    CHECK(tier_for(900.f, 16.f) == Tier::Tight);     // 900p at 200 %
    CHECK(tier_for(512.f, 8.f) == Tier::Compact && tier_for(511.f, 8.f) == Tier::Tight);   // the desk pane at 1024x600 is about 440 tall: tight

    /* ---- header plan ---- */
    HeaderIn in;
    in.u = 8.f;
    in.play_w = 136.f; in.online_w = 146.f; in.more_w = 106.f; in.close_w = 106.f;   // full row = 494 + 3 gaps of 10 = 524

    /* 1920x1080 page: big cover, labels under the title */
    in.avail_w = 1872.f; in.sheet_h = 1080.f;
    HeaderPlan p = plan_header(in);
    CHECK(p.tier == Tier::Full && near(p.cover_h, 128.f) && near(p.cover_w, 166.4f) && near(p.text_x, 186.4f));
    CHECK(!p.beside && !p.icons && p.title_lines == 2 && p.time_line && near(p.buttons_w, 524.f) && near(p.btn_h, 44.f));

    /* 1280x720 page: compact header, labels beside the title */
    in.avail_w = 1232.f; in.sheet_h = 720.f;
    p = plan_header(in);
    CHECK(p.tier == Tier::Compact && near(p.cover_h, 68.f) && p.beside && !p.icons && p.title_lines == 1 && p.time_line);
    CHECK(p.cover_h < 128.f && p.btn_h < 44.f);   // smaller than the full header

    /* the desk pane at 1280x720: no Close; three labels still fit beside the title */
    HeaderIn pn = in;
    pn.close_w = 0.f; pn.avail_w = 800.f; pn.sheet_h = 640.f;
    p = plan_header(pn);
    CHECK(p.beside && !p.icons && near(p.buttons_w, 408.f));
    /* a long title that the labels would cut but the icons leave whole: icons */
    pn.title_w = 400.f;   // 691.6 - 408 - 16 = 267.6 < 400 <= 691.6 - 136 - 100 - 16 = 439.6
    p = plan_header(pn);
    CHECK(p.beside && p.icons);
    pn.title_w = 500.f;   // cut either way: keep the labels
    p = plan_header(pn);
    CHECK(p.beside && !p.icons);
    pn.title_w = 200.f;   // fits beside the labels
    p = plan_header(pn);
    CHECK(p.beside && !p.icons);
    pn.title_w = 0.f;
    /* the same width with a Close button: the labels give way to icons, still beside the title */
    pn.close_w = 106.f;
    p = plan_header(pn);
    CHECK(p.beside && p.icons && near(p.buttons_w, 136.f + 3.f * (40.f + 10.f)));
    CHECK(pn.avail_w - p.text_x - p.buttons_w - 16.f >= title_min_w(8.f));   // the title keeps its minimum

    /* 1024x600 at 150 %: tight, no cover, icons beside the title, play time joins the facts line */
    HeaderIn t;
    t.u = 12.f; t.avail_w = 976.f; t.sheet_h = 600.f;
    t.play_w = 144.f; t.online_w = 194.f; t.more_w = 149.f; t.close_w = 154.f;
    p = plan_header(t);
    CHECK(p.tier == Tier::Tight && p.cover_h == 0.f && p.text_x == 0.f && p.beside && p.icons && !p.time_line && near(p.btn_h, 54.f));

    /* very narrow: no cover, the buttons go under the title as icons */
    in.avail_w = 400.f; in.sheet_h = 700.f;
    p = plan_header(in);
    CHECK(p.cover_h == 0.f && !p.beside && p.icons && p.title_lines == 2);

    /* full tier but a narrow text column: icons under the title */
    in.avail_w = 600.f; in.sheet_h = 900.f;
    p = plan_header(in);
    CHECK(p.tier == Tier::Full && p.cover_h > 0.f && !p.beside && p.icons);

    /* ---- body ---- */
    CHECK(near(readable_w(1872.f, 8.f), 896.f));
    CHECK(near(readable_w(700.f, 8.f), 700.f));
    CHECK(near(readable_w(1872.f, 12.f), 1344.f));   // scales with the UI size

    AboutCols c = about_columns(1840.f, 8.f);   // 1920 wide page
    CHECK(c.side && near(c.side_w, 448.f) && near(c.main_w, 896.f) && near(c.side_x, 928.f));
    c = about_columns(1196.f, 8.f);             // 1280 wide page
    CHECK(c.side && c.main_w >= 640.f && near(c.side_x, c.main_w + 32.f) && c.side_x + c.side_w <= 1196.f + 0.01f);
    c = about_columns(960.f, 8.f);              // just wide enough: the main column keeps its minimum
    CHECK(c.side && near(c.main_w, 640.f) && near(c.side_w, 288.f));
    c = about_columns(940.f, 8.f);              // 1024 wide page, or the desk pane: one column
    CHECK(!c.side && near(c.main_w, 896.f));
    c = about_columns(500.f, 8.f);
    CHECK(!c.side && near(c.main_w, 500.f));
    for (float w = 300.f; w < 3000.f; w += 37.f) {   // never wider than the body, the main column never squeezed
        const AboutCols k = about_columns(w, 8.f);
        CHECK(k.main_w <= w + 0.01f);
        if (k.side) CHECK(k.main_w >= 640.f - 0.01f && k.side_w >= 288.f - 0.01f && k.side_x + k.side_w <= w + 0.01f);
    }

    /* ---- hosts ---- */
    const HostMetrics ov = host_metrics(false), pa = host_metrics(true);
    CHECK(ov.close_button && ov.footer_hints && !ov.keep_tab);
    CHECK(!pa.close_button && !pa.footer_hints && pa.keep_tab);

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_gamepage_test ok\n");
    return 0;
}
