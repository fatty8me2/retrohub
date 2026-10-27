/* RetroHub Wrapped (Wave 3 L8). A monthly / yearly "story" built from the play history the Your stats card already uses
 * (the per-session play log in the vault, plus the recent online groups for the most-played-with friend):
 * five cards (time, top games, consoles, streaks, played with). Reached from the Your stats card.
 * Pad: Left / Right move between the Previous / Next / Save / Close buttons, LB / RB (or the arrow keys) change the card, A presses, B closes.
 * "Save as image" paints all cards plus a title tile onto one sheet and writes it as a PNG into the shots folder of the
 * top game of the period (<vault>/<system>/<game key>/shots/wrapped-<period>.png). The aggregation is pure (wrapped_logic.h, unit tested).
 * Screenshot aids (headless only): HUB_SHOT_WRAPPED=month|year opens it, HUB_SHOT_WRAPPED_CARD=0-4, HUB_SHOT_WRAPPED_FAKE=1 uses made-up
 * sessions and friends over the fake library, HUB_SHOT_WRAPPED_SAVE=1 saves the image once (path goes to HUB_SHOT_TRACE). */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../retrorun/third_party/stb_image_write.h"
#include "app.h"
#include "homefun.h"
#include "wrapped_logic.h"

namespace {

namespace wr = hubui::wrapped;
namespace hs = hub::saves;

constexpr float CW = 360.f, CH = 480.f;   // logical card size; everything is laid out in these units and scaled by k
constexpr int kCards = 5;

struct Wrapped {
    bool want_open = false, open = false;
    wr::Period period = wr::Period::Month;
    int back = 0;          // periods before the current one
    int card = 0;
    bool dirty = true;
    bool auto_back_done = false;
    wr::Data data;
    std::string status;
    bool status_ok = true;
    std::vector<hs::Session> fake_sessions;
    std::vector<wr::GroupEvent> fake_groups;
    bool use_fake = false;
    std::string shown_key;   // game whose cover the Top games card shows
} WS;

int local_tz() {
    std::time_t t = std::time(nullptr);
    std::tm lt{}, gm{};
    localtime_s(&lt, &t);
    gmtime_s(&gm, &t);
    int tz = (int)std::difftime(std::mktime(&lt), std::mktime(&gm));
    if (lt.tm_isdst > 0) tz += 3600;
    return tz;
}

std::string title_of(const std::string& key) {
    if (const hub::GameEntry* e = find_entry(key)) return e->title;
    return key;
}

std::string sys_name(const std::string& sys_short_name) { return hub::system_name(hub::system_from_short(sys_short_name)); }

void make_fakes() {
    WS.use_fake = true;
    WS.fake_sessions.clear();
    WS.fake_groups.clear();
    std::vector<const hub::GameEntry*> es;
    for (auto& e : A.lib) { es.push_back(&e); if (es.size() >= 6) break; }
    if (es.empty()) return;
    const int64_t now = (int64_t)unix_now();
    const int tz = local_tz();
    const wr::Range mo = wr::period_range(wr::Period::Month, now, tz);
    const int64_t day0 = mo.from - 60 * 86400;   // the last two months and this one
    uint32_t s = 7;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return s >> 8; };
    for (int d = 0; d < 125; ++d) {
        if (d % 9 == 4 || d % 9 == 5) continue;   // some gaps, plus a streak of seven or so in between
        const int n = 1 + (int)(rnd() % 2);
        for (int j = 0; j < n; ++j) {
            const hub::GameEntry* e = es[(rnd() % 100 < 45) ? 0 : (rnd() % es.size())];
            hs::Session x;
            x.system = sys_short(*e);
            x.key = e->key;
            x.start = day0 + (int64_t)d * 86400 + 18 * 3600 + j * 5400;
            x.secs = 1200 + (int64_t)(rnd() % 6000);
            if (x.start < now) WS.fake_sessions.push_back(x);
        }
    }
    WS.fake_groups.push_back({now - 2 * 86400, {{"u1", "Mia"}, {"u2", "Jake"}}});
    WS.fake_groups.push_back({now - 5 * 86400, {{"u1", "Mia"}}});
    WS.fake_groups.push_back({now - 9 * 86400, {{"u3", "Kai"}, {"u1", "Mia"}}});
}

void recompute() {
    const int tz = local_tz();
    const int64_t now = (int64_t)unix_now();
    std::vector<hs::Session> sessions = WS.use_fake ? WS.fake_sessions : hs::playlog_read_all();
    std::vector<wr::GroupEvent> groups;
    if (WS.use_fake) groups = WS.fake_groups;
    else
        for (auto& g : GRP.groups.v) {
            wr::GroupEvent ev;
            ev.at_s = g.at / 1000;
            for (auto& m : g.members) ev.members.push_back({m.uid, m.name});
            groups.push_back(ev);
        }
    WS.data = wr::aggregate(sessions, groups, WS.period, wr::period_range(WS.period, now, tz, WS.back), tz);
    if (!WS.auto_back_done) {   // first view of an empty month: show last month instead
        WS.auto_back_done = true;
        if (WS.period == wr::Period::Month && WS.back == 0 && WS.data.empty()) {
            wr::Data prev = wr::aggregate(sessions, groups, WS.period, wr::period_range(WS.period, now, tz, 1), tz);
            if (!prev.empty()) { WS.back = 1; WS.data = prev; }
        }
    }
    WS.dirty = false;
}

std::string date_text(int64_t unix_s) {
    int y, m, d;
    wr::civil_from_days(wr::floor_div(unix_s + local_tz(), 86400), &y, &m, &d);
    static const char* mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return std::to_string(d) + " " + mn[(m - 1) % 12];
}

/* ---------------------------------------------------------------- painting */

struct Paint {
    ImDrawList* dl;
    ImVec2 o;      // top-left of the card, pixels
    float k;       // pixels per logical unit
    ImVec2 P(float x, float y) const { return ImVec2(o.x + x * k, o.y + y * k); }
    float tw(ImFont* f, float pt, const std::string& s) const { return f->CalcTextSizeA(pt * k, FLT_MAX, 0.f, s.c_str()).x / k; }
    float fit(ImFont* f, float pt, const std::string& s, float maxw) const { const float w = tw(f, pt, s); return w > maxw ? pt * maxw / w : pt; }
    void text(ImFont* f, float pt, float x, float y, ImU32 col, const std::string& s, float maxw = 0.f) const {
        const std::string t = maxw > 0.f ? ui::ellipsize(f, pt * k, s, maxw * k) : s;
        dl->AddText(f, pt * k, P(x, y), col, t.c_str());
    }
    void ctext(ImFont* f, float pt, float y, ImU32 col, const std::string& s, float maxw = 312.f) const {
        const float size = fit(f, pt, s, maxw);
        dl->AddText(f, size * k, P((CW - tw(f, size, s)) * 0.5f, y), col, s.c_str());
    }
};

void card_bg(const Paint& p, int idx) {
    static const ImU32 c0[kCards] = {IM_COL32(96, 70, 230, 255), IM_COL32(222, 64, 138, 255), IM_COL32(34, 156, 208, 255), IM_COL32(244, 138, 54, 255), IM_COL32(44, 176, 128, 255)};
    static const ImU32 c1[kCards] = {IM_COL32(36, 22, 104, 255), IM_COL32(78, 20, 66, 255), IM_COL32(12, 52, 98, 255), IM_COL32(110, 36, 40, 255), IM_COL32(12, 66, 70, 255)};
    const int i = idx % kCards;
    gradient_round_rect(p.dl, p.P(0, 0), p.P(CW, CH), c0[i], c1[i], 22.f * p.k);
    p.dl->PushClipRect(p.P(0, 0), p.P(CW, CH), true);
    p.dl->AddCircleFilled(p.P(CW * 0.95f, CH * 0.12f), 120.f * p.k, IM_COL32(255, 255, 255, 18), 48);
    p.dl->AddCircleFilled(p.P(CW * 0.05f, CH * 0.92f), 150.f * p.k, IM_COL32(0, 0, 0, 38), 48);
    p.dl->PopClipRect();
    p.dl->AddRect(p.P(0, 0), p.P(CW, CH), IM_COL32(255, 255, 255, 36), 22.f * p.k, 0, std::max(1.f, p.k));
}

void card_frame(const Paint& p, const wr::Data& d, const char* heading, const std::string& player) {
    const ImU32 white = IM_COL32(255, 255, 255, 255), dim = IM_COL32(255, 255, 255, 170);
    p.text(ui::F.cap, 12.f, 24, 24, dim, "RETROHUB WRAPPED");
    p.text(ui::F.bold, 16.f, 24, 40, white, wr::period_label(d.range), 312);
    p.text(ui::F.bold, 15.f, 24, 72, IM_COL32(255, 255, 255, 215), heading, 312);
    p.dl->AddLine(p.P(24, 462), p.P(CW - 24, 462), IM_COL32(255, 255, 255, 40), std::max(1.f, p.k * 0.8f));
    p.text(ui::F.body, 13.f, 24, 444, dim, player.empty() ? std::string("Player") : player, 200);
}

void empty_note(const Paint& p, const wr::Data& d) {
    p.ctext(ui::F.title, 24.f, 190, IM_COL32(255, 255, 255, 255), "Nothing here yet");
    p.ctext(ui::F.body, 14.f, 232, IM_COL32(255, 255, 255, 190), "No play time was logged in " + wr::period_label(d.range) + ".");
    p.ctext(ui::F.body, 14.f, 254, IM_COL32(255, 255, 255, 190), "Play a game and come back!");
}

void paint_card(const Paint& p, int idx, const wr::Data& d, const std::string& player) {
    const ImU32 white = IM_COL32(255, 255, 255, 255), dim = IM_COL32(255, 255, 255, 185);
    card_bg(p, idx);
    static const char* heading[kCards] = {"Your time", "Your top games", "Your consoles", "Your streaks", "Played with"};
    card_frame(p, d, heading[idx % kCards], player);
    if (d.empty()) { empty_note(p, d); return; }
    switch (idx % kCards) {
        case 0: {
            p.ctext(ui::F.bold, 14.f, 116, dim, "YOU PLAYED FOR");
            p.ctext(ui::F.hero, 56.f, 140, white, wr::hours_text(d.total_secs));
            p.ctext(ui::F.body, 15.f, 214, dim, "across " + std::to_string(d.games_played) + (d.games_played == 1 ? " game" : " games"));
            struct Cell { std::string big, label; };
            const Cell cells[3] = {{std::to_string(d.sessions), d.sessions == 1 ? "session" : "sessions"},
                                   {std::to_string(d.active_days), d.active_days == 1 ? "day played" : "days played"},
                                   {wr::hours_text(d.sessions ? d.total_secs / d.sessions : 0), "per session"}};
            for (int i = 0; i < 3; ++i) {
                const float x = 24 + i * 108.f;
                p.dl->AddRectFilled(p.P(x, 290), p.P(x + 96, 380), IM_COL32(0, 0, 0, 70), 14.f * p.k);
                const float sz = p.fit(ui::F.title, 24.f, cells[i].big, 84);
                p.text(ui::F.title, sz, x + 48 - p.tw(ui::F.title, sz, cells[i].big) * 0.5f, 306, white, cells[i].big);
                p.text(ui::F.cap, 12.5f, x + 48 - p.tw(ui::F.cap, 12.5f, cells[i].label) * 0.5f, 344, dim, cells[i].label);
            }
            break;
        }
        case 1: {
            // banner for #1
            const wr::GameSecs& g = d.top[0];
            const hub::GameEntry* e = find_entry(g.key);
            const ImVec2 b0 = p.P(24, 98), b1 = p.P(CW - 24, 214);
            ImU32 c0 = ui::PANEL2, c1 = ui::PANEL;
            if (e) entry_colors(*e, &c0, &c1);
            gradient_round_rect(p.dl, b0, b1, c0, c1, 16.f * p.k);
            if (e) {
                ArtRef art = art_for_entry(*e);
                if (art.tex) ui::draw_art_bg(p.dl, art.tex, art.w, art.h, b0, b1, 16.f * p.k, 0.85f);
            }
            ui::vfade_round_rect(p.dl, b0, b1, IM_COL32(8, 8, 12, 255), 16.f * p.k, 0, 0.0f, 0.85f);
            p.text(ui::F.cap, 11.f, 38, 106, IM_COL32(255, 255, 255, 230), "#1");
            p.text(ui::F.bold, 17.f, 38, 166, white, title_of(g.key), 284);
            p.text(ui::F.body, 12.f, 38, 188, dim, wr::hours_text(g.secs) + "  \xC2\xB7  " + std::to_string(g.sessions) + (g.sessions == 1 ? " session" : " sessions"));
            for (size_t i = 1; i < d.top.size() && i < 5; ++i) {
                const wr::GameSecs& t = d.top[i];
                const float y = 230 + (float)(i - 1) * 54.f;
                p.text(ui::F.title, 22.f, 28, y + 4, IM_COL32(255, 255, 255, 200), std::to_string(i + 1));
                p.text(ui::F.bold, 15.f, 62, y, white, title_of(t.key), 210);
                const float frac = d.top[0].secs ? (float)t.secs / (float)d.top[0].secs : 0.f;
                p.dl->AddRectFilled(p.P(62, y + 24), p.P(62 + 190, y + 30), IM_COL32(0, 0, 0, 80), 3.f * p.k);
                p.dl->AddRectFilled(p.P(62, y + 24), p.P(62 + std::max(6.f, 190.f * frac), y + 30), IM_COL32(255, 255, 255, 220), 3.f * p.k);
                p.text(ui::F.body, 13.f, 262, y + 20, dim, wr::hours_text(t.secs), 74);
            }
            break;
        }
        case 2: {
            const std::string top_sys = sys_name(d.systems[0].first);
            p.ctext(ui::F.title, 30.f, 104, white, "Mostly " + top_sys);
            const float bx = 24, bw = 312, by = 160, bh = 24;
            float x = bx;
            for (size_t i = 0; i < d.systems.size(); ++i) {
                ImU32 a, b;
                system_colors(hub::system_from_short(d.systems[i].first), &a, &b);
                const float w = bw * (float)d.systems[i].second / (float)std::max<int64_t>(1, d.total_secs);
                if (w < 1.f) continue;
                p.dl->AddRectFilled(p.P(x, by), p.P(std::min(bx + bw, x + w), by + bh), ui::mix(a, IM_COL32_WHITE, 0.15f), i == 0 ? 8.f * p.k : 0.f);
                x += w;
            }
            for (size_t i = 0; i < d.systems.size() && i < 6; ++i) {
                ImU32 a, b;
                system_colors(hub::system_from_short(d.systems[i].first), &a, &b);
                const float y = 210 + (float)i * 38.f;
                const int pct = (int)(d.systems[i].second * 100 / std::max<int64_t>(1, d.total_secs));
                p.dl->AddCircleFilled(p.P(34, y + 10), 7.f * p.k, ui::mix(a, IM_COL32_WHITE, 0.15f), 20);
                p.text(ui::F.bold, 15.f, 52, y, white, sys_name(d.systems[i].first), 150);
                p.text(ui::F.body, 13.f, 210, y + 1, dim, wr::hours_text(d.systems[i].second), 80);
                const std::string ps = std::to_string(pct) + "%";
                p.text(ui::F.bold, 14.f, 336 - p.tw(ui::F.bold, 14.f, ps), y, white, ps);
            }
            break;
        }
        case 3: {
            p.ctext(ui::F.hero, 96.f, 100, white, std::to_string(d.longest_streak));
            p.ctext(ui::F.bold, 17.f, 212, white, d.longest_streak == 1 ? "day in a row" : "days in a row");
            p.ctext(ui::F.body, 13.f, 236, dim, "your longest streak");
            struct Row { std::string k, v; };
            std::vector<Row> rows;
            rows.push_back({"Longest session", wr::hours_text(d.longest_secs) + "  (" + date_text(d.longest_start) + ")"});
            if (d.busiest_weekday >= 0) rows.push_back({"Busiest day", std::string(wr::weekday_name(d.busiest_weekday))});
            rows.push_back({"Days played", std::to_string(d.active_days)});
            for (size_t i = 0; i < rows.size(); ++i) {
                const float y = 290 + (float)i * 44.f;
                p.dl->AddRectFilled(p.P(24, y), p.P(CW - 24, y + 36), IM_COL32(0, 0, 0, 70), 12.f * p.k);
                p.text(ui::F.body, 13.f, 38, y + 10, dim, rows[i].k);
                const float sz = p.fit(ui::F.bold, 14.f, rows[i].v, 170);
                p.text(ui::F.bold, sz, CW - 38 - p.tw(ui::F.bold, sz, rows[i].v), y + 10, white, rows[i].v);
            }
            break;
        }
        default: {
            if (d.has_friend) {
                const std::string nm = d.best_friend.name.empty() ? std::string("A friend") : d.best_friend.name;
                const ImVec2 c = p.P(CW * 0.5f, 150);
                p.dl->AddCircleFilled(c, 46.f * p.k, IM_COL32(0, 0, 0, 80), 40);
                p.dl->AddCircleFilled(c, 40.f * p.k, IM_COL32(255, 255, 255, 235), 40);
                char init[8] = {(char)std::toupper((unsigned char)nm[0]), 0};
                const ImVec2 ts = ui::F.hero->CalcTextSizeA(44.f * p.k, FLT_MAX, 0.f, init);
                p.dl->AddText(ui::F.hero, 44.f * p.k, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(40, 70, 70, 255), init);
                p.ctext(ui::F.cap, 12.f, 216, dim, "YOUR MOST PLAYED-WITH FRIEND");
                p.ctext(ui::F.hero, 44.f, 238, white, nm);
                const int n = d.best_friend.together;
                p.ctext(ui::F.bold, 17.f, 302, white, std::to_string(n) + (n == 1 ? " online session together" : " online sessions together"));
                p.ctext(ui::F.body, 12.f, 336, dim, "from your recent online sessions");
            } else {
                p.ctext(ui::F.title, 24.f, 170, white, "Nobody yet");
                p.ctext(ui::F.body, 14.f, 212, dim, "Play online with a friend and");
                p.ctext(ui::F.body, 14.f, 232, dim, "they will show up here.");
            }
            break;
        }
    }
}

/* The title tile of the exported sheet. */
void paint_title_tile(const Paint& p, const wr::Data& d, const std::string& player) {
    gradient_round_rect(p.dl, p.P(0, 0), p.P(CW, CH), IM_COL32(26, 28, 44, 255), IM_COL32(12, 12, 20, 255), 22.f * p.k);
    p.dl->AddRect(p.P(0, 0), p.P(CW, CH), IM_COL32(255, 255, 255, 36), 22.f * p.k, 0, std::max(1.f, p.k));
    ui_draw_brand_logo(p.dl, p.P(CW * 0.5f - 40, 70), 80.f * p.k);
    p.ctext(ui::F.title, 30.f, 176, IM_COL32(255, 255, 255, 255), "RetroHub Wrapped");
    p.ctext(ui::F.bold, 20.f, 218, ui::ACCENT, wr::period_label(d.range));
    if (!d.empty()) {
        p.ctext(ui::F.hero, 52.f, 280, IM_COL32(255, 255, 255, 255), wr::hours_text(d.total_secs));
        p.ctext(ui::F.body, 14.f, 346, IM_COL32(255, 255, 255, 170), "played  \xC2\xB7  " + std::to_string(d.sessions) + (d.sessions == 1 ? " session" : " sessions"));
    }
    p.ctext(ui::F.body, 14.f, 430, IM_COL32(255, 255, 255, 200), player.empty() ? std::string("Player") : player);
}

/* ---------------------------------------------------------------- export */

constexpr int kSheetCols = 3, kSheetRows = 2;
constexpr float kSheetK = 1.5f, kSheetGap = 24.f;

/* Paints the sheet into a private draw list and renders it to an offscreen texture; fills rgb (top-down, 3 bytes per pixel). */
bool render_sheet(const wr::Data& d, const std::string& player, int* out_w, int* out_h, std::vector<unsigned char>* rgb) {
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (!ren) return false;
    const float cwp = CW * kSheetK, chp = CH * kSheetK;
    const int W_ = (int)(kSheetCols * cwp + (kSheetCols + 1) * kSheetGap), H_ = (int)(kSheetRows * chp + (kSheetRows + 1) * kSheetGap);

    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushTextureID(ImGui::GetIO().Fonts->TexID);
    dl.PushClipRect(ImVec2(0, 0), ImVec2((float)W_, (float)H_));
    dl.AddRectFilled(ImVec2(0, 0), ImVec2((float)W_, (float)H_), ui::BG);
    for (int i = 0; i < kSheetCols * kSheetRows; ++i) {
        const int col = i % kSheetCols, row = i / kSheetCols;
        Paint p{&dl, ImVec2(kSheetGap + col * (cwp + kSheetGap), kSheetGap + row * (chp + kSheetGap)), kSheetK};
        if (i == 0) paint_title_tile(p, d, player);
        else paint_card(p, i - 1, d, player);
    }
    dl._PopUnusedDrawCmd();

    ImDrawData dd;
    dd.Valid = true;
    dd.CmdListsCount = 1;
    dd.TotalVtxCount = dl.VtxBuffer.Size;
    dd.TotalIdxCount = dl.IdxBuffer.Size;
    dd.CmdLists.push_back(&dl);
    dd.DisplayPos = ImVec2(0, 0);
    dd.DisplaySize = ImVec2((float)W_, (float)H_);
    dd.FramebufferScale = ImVec2(1, 1);

    SDL_Texture* rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, W_, H_);
    if (!rt) return false;
    SDL_Texture* old = SDL_GetRenderTarget(ren);
    SDL_SetRenderTarget(ren, rt);
    SDL_SetRenderDrawColor(ren, 16, 17, 22, 255);
    SDL_RenderClear(ren);
    ImGui_ImplSDLRenderer2_RenderDrawData(&dd, ren);
    std::vector<unsigned char> px((size_t)W_ * H_ * 4);
    const bool ok = SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), W_ * 4) == 0;
    SDL_SetRenderTarget(ren, old);
    SDL_DestroyTexture(rt);
    dd.CmdLists.clear();
    if (!ok) return false;
    rgb->resize((size_t)W_ * H_ * 3);
    for (size_t i = 0; i < (size_t)W_ * H_; ++i) {
        (*rgb)[i * 3 + 0] = px[i * 4 + 0];
        (*rgb)[i * 3 + 1] = px[i * 4 + 1];
        (*rgb)[i * 3 + 2] = px[i * 4 + 2];
    }
    *out_w = W_;
    *out_h = H_;
    return true;
}

void to_file(void* ctx, void* data, int size) { std::fwrite(data, 1, (size_t)size, (FILE*)ctx); }

/* Writes the sheet; returns the path ("" on failure). */
std::string save_image(const wr::Data& d, const std::string& player, std::string* err) {
    if (d.empty() || d.top.empty()) { *err = "There is nothing to save for this period yet."; return std::string(); }
    const std::string& key = d.top[0].key;
    const std::string sys = d.top[0].system;
    const std::string dir = no_slash(hs::game_dir(sys, key)) + "\\shots";
    make_dirs(dir);
    char nm[64];
    if (d.range.month) std::snprintf(nm, sizeof nm, "wrapped-%04d-%02d.png", d.range.year, d.range.month);
    else std::snprintf(nm, sizeof nm, "wrapped-%04d.png", d.range.year);
    const std::string path = dir + "\\" + nm;
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
    if (!render_sheet(d, player, &w, &h, &rgb)) { *err = "Could not draw the picture."; return std::string(); }
    FILE* f = _wfopen(std::filesystem::u8path(path).c_str(), L"wb");
    if (!f) { *err = "Could not write " + path; return std::string(); }
    const int ok = stbi_write_png_to_func(to_file, f, w, h, 3, rgb.data(), w * 3);
    std::fclose(f);
    if (!ok) { *err = "Could not write the picture."; return std::string(); }
    return path;
}

void select_period(wr::Period p) {
    if (WS.period == p) return;
    WS.period = p;
    WS.back = 0;
    WS.auto_back_done = true;
    WS.dirty = true;
}

}  // namespace

void wrapped_ui_open() {
    WS.want_open = true;
    WS.card = 0;
    WS.back = 0;
    WS.period = wr::Period::Month;
    WS.auto_back_done = false;
    WS.dirty = true;
    WS.status.clear();
}

void wrapped_ui_shot_init() {
    if (!g_headless) return;
    if (const char* w = std::getenv("HUB_SHOT_WRAPPED")) {
        if (std::getenv("HUB_SHOT_WRAPPED_FAKE")) make_fakes();
        wrapped_ui_open();
        if (std::string(w) == "year") { WS.period = wr::Period::Year; WS.auto_back_done = true; }
        if (const char* c = std::getenv("HUB_SHOT_WRAPPED_CARD")) WS.card = std::max(0, std::min(kCards - 1, std::atoi(c)));
    }
}

void wrapped_ui_modal() {
    if (g_headless && std::getenv("HUB_SHOT_WRAPPED") && !WS.open && !WS.want_open) {   // opened by the aid once the library is up
        static bool opened = false;
        if (!opened && SH.el > 1.2 && !A.lib.empty()) { opened = true; wrapped_ui_shot_init(); }
    }
    if (WS.want_open) { WS.want_open = false; WS.open = true; ImGui::OpenPopup("##wrapped"); }
    if (!WS.open) return;
    ImGuiIO& io = ImGui::GetIO();
    const float k = std::max(0.55f, std::min(1.5f, (io.DisplaySize.y * 0.92f - sp(30.f)) / CH));
    const float pw = CW * k + sp(6.f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(std::max(pw, sp(72.f)), io.DisplaySize.x * 0.94f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    const bool open = ImGui::BeginPopupModal("##wrapped", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) { WS.open = false; return; }
    if (ui::modal_back()) { WS.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    if (WS.dirty) recompute();

    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted("RetroHub Wrapped");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    // period: Month / Year, with earlier / later arrows
    {
        static const char* labels[] = {"Month", "Year"};
        const int cur = WS.period == wr::Period::Month ? 0 : 1;
        const int n = ui::segmented("wperiod", labels, 2, cur);
        if (n != cur) select_period(n == 0 ? wr::Period::Month : wr::Period::Year);
        ImGui::SameLine(0, sp(1.5f));
        const float ib = sp(4.f);
        if (ui::icon_button("wearlier", ui::Icon::ChevronL, ib, false, "Earlier")) { WS.back++; WS.dirty = true; WS.auto_back_done = true; }
        ImGui::SameLine(0, sp(0.75f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(wr::period_label(WS.data.range).c_str());
        ImGui::SameLine(0, sp(0.75f));
        ImGui::BeginDisabled(WS.back == 0);
        if (ui::icon_button("wlater", ui::Icon::ChevronR, ib, false, "Later")) { WS.back = std::max(0, WS.back - 1); WS.dirty = true; WS.auto_back_done = true; }
        ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(0, sp(0.75f)));

    // the card
    const std::string player = A.st.name;
    {
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const ImVec2 co(o.x + (avail - CW * k) * 0.5f, o.y);
        Paint p{ImGui::GetWindowDrawList(), co, k};
        ui::shadow(p.dl, p.P(0, 0), p.P(CW, CH), 22.f * k, 1.2f);
        paint_card(p, WS.card, WS.data, player);
        ImGui::Dummy(ImVec2(avail, CH * k));
        // dots
        const ImVec2 d0 = ImGui::GetCursorScreenPos();
        const float gap = sp(1.5f), r = sp(0.45f);
        float x = d0.x + (avail - (kCards - 1) * gap) * 0.5f;
        for (int i = 0; i < kCards; ++i, x += gap)
            p.dl->AddCircleFilled(ImVec2(x, d0.y + sp(1.f)), i == WS.card ? r * 1.5f : r, i == WS.card ? ui::TEXT : ui::TEXT3, 12);
        ImGui::Dummy(ImVec2(avail, sp(2.f)));
    }

    // buttons
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) WS.card = (WS.card + kCards - 1) % kCards;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) WS.card = (WS.card + 1) % kCards;
    const float bh = sp(5.f);
    if (button("Previous", ImVec2(0, bh), false, true, ui::Icon::ChevronL)) WS.card = (WS.card + kCards - 1) % kCards;
    ImGui::SameLine(0, sp(1.f));
    if (button("Next", ImVec2(0, bh), true, true, ui::Icon::ChevronR)) WS.card = (WS.card + 1) % kCards;
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, sp(1.f));
    if (button("Save as image", ImVec2(0, bh), false, !WS.data.empty())) {
        std::string err;
        const std::string path = save_image(WS.data, player, &err);
        WS.status_ok = !path.empty();
        WS.status = path.empty() ? err : "Saved to " + path;
        if (!path.empty()) set_toast("Saved your Wrapped picture to the screenshots of " + title_of(WS.data.top[0].key) + ".", true, 6.0);
        shot_trace("wrapped-save", path.empty() ? "failed: " + err : path);
    }
    ImGui::SameLine(0, sp(1.f));
    if (button("Close", ImVec2(0, bh), false, true, ui::Icon::Close)) { WS.open = false; ImGui::CloseCurrentPopup(); }
    if (!WS.status.empty()) result_line(WS.status, WS.status_ok);
    else {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextUnformatted("LB / RB  change card      B  close");
        ImGui::PopStyleColor();
    }
    if (g_headless && std::getenv("HUB_SHOT_WRAPPED_SAVE")) {
        static bool saved = false;
        if (!saved && SH.el > 2.0 && !WS.data.empty()) {
            saved = true;
            std::string err;
            const std::string path = save_image(WS.data, player, &err);
            WS.status_ok = !path.empty();
            WS.status = path.empty() ? err : "Saved to " + path;
            shot_trace("wrapped-save", path.empty() ? "failed: " + err : path);
        }
    }
    ImGui::EndPopup();
}
