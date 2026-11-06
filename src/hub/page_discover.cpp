#include "app.h"
#include "art.h"
#include "discover_logic.h"
#include "gamepack.h"

/* Discover: curated, highly rated games per console (data/recommended.json). Each card says where the game stands:
 *   in your library -> Play;  in a shared pack -> Install (the GamePack queue, the only install path);
 *   otherwise -> "Add your own copy to play" and the normal Add games dialog. Nothing here downloads or links to game files.
 * Cover art for games you do not own is resolved the same way as for pack ghosts (art_for_ghost). */

namespace {

std::string g_sys;          // selected console short code ("" = pick a default on first draw)
bool g_friends = false;     // only "Great with friends"

struct Cached {
    discover::Rec const* rec = nullptr;
    discover::Match m;
};
std::vector<Cached> g_rows;
std::string g_sig;

const char* short_label(const std::string& s)
{
    if (s == "nes") return "NES";
    if (s == "snes") return "SNES";
    if (s == "n64") return "N64";
    if (s == "gb") return "Game Boy";
    if (s == "gbc") return "GB Color";
    if (s == "gba") return "GBA";
    if (s == "nds") return "DS";
    if (s == "3ds") return "3DS";
    if (s == "gc") return "GameCube";
    if (s == "wiiu") return "Wii U";
    if (s == "ps1") return "PS1";
    if (s == "ps2") return "PS2";
    if (s == "psp") return "PSP";
    if (s == "switch") return "Switch";
    return s.c_str();
}

/* Word-wrap `text` into at most `max_lines` lines of width `w`; the last line is ellipsized when text is left over. */
std::vector<std::string> wrap_lines(ImFont* f, float size, const std::string& text, float w, int max_lines)
{
    std::vector<std::string> out;
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end && (int)out.size() < max_lines) {
        while (p < end && *p == ' ') ++p;
        if (p >= end) break;
        const char* brk = f->CalcWordWrapPositionA(size / f->FontSize, p, end, w);
        if (brk <= p) brk = p + 1;
        std::string line(p, brk);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        p = brk;
        if ((int)out.size() == max_lines - 1 && p < end) line = ui::ellipsize(f, size, line + " " + std::string(p, end), w);
        out.push_back(line);
    }
    return out;
}

/* Screenshots (HUB_SHOT_DISCOVER_FAKE=1, headless): a made-up library that owns every 5th recommendation and a made-up shared pack
 * that holds the 2nd and 4th of each five, so all three card states show. Never touches files or the network. */
void shot_seed(const std::vector<discover::Rec>& recs)
{
    if (!g_headless || !std::getenv("HUB_SHOT_DISCOVER_FAKE")) return;
    for (const auto& e : A.lib) if (e.key.rfind("fake:disc:", 0) == 0) return;
    std::vector<hubgp::PackGame> pack;
    for (const std::string& c : discover::consoles()) {
        std::vector<const discover::Rec*> v = discover::for_console(recs, c, false);
        for (size_t i = 0; i < v.size(); ++i) {
            if (i % 5 == 0) {
                hub::GameEntry g;
                g.system = hub::system_from_short(c);
                g.key = "fake:disc:" + c + std::to_string(i);
                g.title = v[i]->title;
                g.catalog_id = v[i]->cid;
                g.path = "C:/fake/" + g.key + ".rom";
                A.lib.push_back(g);
            } else if (i % 5 == 1 || i % 5 == 3) {
                pack.push_back({c, v[i]->title, hubgp::pack_key(v[i]->title), v[i]->title + " (USA)"});
            }
        }
    }
    hubgp::index_test_set(pack);
}

void refresh_rows(const std::vector<discover::Rec>& recs)
{
    shot_seed(recs);
    char sig[160];
    std::snprintf(sig, sizeof sig, "%s|%d|%zu|%u|%zu", g_sys.c_str(), g_friends ? 1 : 0, A.lib.size(), hubgp::index_epoch(), recs.size());
    if (g_sig == sig) return;
    g_sig = sig;
    g_rows.clear();
    const std::vector<hubui::fg::Owned> owned = owned_snapshot();
    const std::vector<hubgp::PackGame> pack = hubgp::index_games();
    for (const discover::Rec* r : discover::for_console(recs, g_sys, g_friends)) g_rows.push_back({r, discover::classify(*r, owned, pack)});
}

void card(const Cached& c, int idx, ImVec2 size)
{
    const discover::Rec& r = *c.rec;
    const discover::Match& m = c.m;
    const GameInfo* cat = r.cid.empty() ? nullptr : hub::find_game(r.cid);
    hub::System sy = hub::system_from_short(r.sys);
    ImU32 c0, c1;
    system_colors(sy, &c0, &c1);

    ImGui::PushID(idx);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::HAIRLINE);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sp(2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.5f)));
    ImGui::BeginChild("card", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float pad = sp(1.5f);
    const float inner_h = size.y - 2 * pad, inner_w = size.x - 2 * pad;

    /* cover: the owned copy's art, else the ghost lookup (catalog name / DAT title / fuzzy title), else the no-art placeholder */
    const float aw = sp(11.f), ah = std::min(inner_h, sp(14.5f));
    ImVec2 a0 = o, a1(o.x + aw, o.y + ah);
    {
        ArtRef art;
        if (ImGui::IsRectVisible(a0, a1)) {
            if (m.st == discover::Status::Owned) {
                if (const hub::GameEntry* e = find_entry(m.key)) art = art_for_entry(*e);
            } else {
                hubui::fg::Ghost g;
                g.sys = m.st == discover::Status::InPack ? m.pack_sys : r.sys;
                g.cid = r.cid;
                g.title = r.title;
                g.hint = m.pack_hint;
                g.id = hubui::fg::identity(g.sys, g.cid, g.title);
                art = art_for_ghost(g);
            }
        }
        const bool own = m.st == discover::Status::Owned;
        ui::shadow(dl, a0, a1, sp(1.25f), 0.8f);
        if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.25f), own ? 1.f : 0.82f);
        else {
            TileSpec ts;
            ts.title = r.title;
            ts.c0 = own ? c0 : mix(c0, IM_COL32(92, 95, 104, 255), 0.6f);
            ts.c1 = own ? c1 : mix(c1, IM_COL32(70, 73, 82, 255), 0.6f);
            draw_no_art(dl, ts, a0, a1, sp(1.25f), 0.85f);
            if (art.loading) ui::shimmer(dl, a0, a1, sp(1.25f), IM_COL32(0, 0, 0, 0));
        }
        dl->AddRect(a0, a1, ui::HAIRLINE, sp(1.25f), 0, 1.f);
    }

    const float tx = o.x + aw + sp(1.5f), tw = inner_w - aw - sp(1.5f);
    float y = o.y;
    ImFont* hf = ui::F.bold;
    dl->AddText(hf, hf->FontSize, ImVec2(tx, y), ui::TEXT, ui::ellipsize(hf, hf->FontSize, r.title, tw).c_str());
    y += hf->FontSize + sp(0.5f);
    {
        std::string meta = std::to_string(r.year) + "  -  " + r.genre + "  -  " + discover::players_text(r.players);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, y), ui::TEXT2, ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, meta, tw).c_str());
        y += ui::F.cap->FontSize + sp(0.75f);
    }
    {   // tags
        float x = tx;
        const float ph = ui::F.cap->FontSize + sp(1.f);
        if (r.friends) x += ui::pill_at(dl, ImVec2(x, y), "Great with friends", mix(ui::GOOD, IM_COL32_BLACK, 0.25f)) + sp(0.5f);
        if (cat && cat->online) x += ui::pill_at(dl, ImVec2(x, y), "Online", mix(ui::ACCENT, IM_COL32_BLACK, 0.2f));
        if (r.friends || (cat && cat->online)) y += ph + sp(0.5f);
    }
    {   // why it is great
        ImFont* bf = ui::F.cap;
        const float lh = bf->FontSize * 1.25f;
        const float footer_h = sp(5.25f);
        int lines = (int)std::max(1.f, std::min(3.f, (o.y + inner_h - footer_h - y) / lh));
        for (const std::string& ln : wrap_lines(bf, bf->FontSize, r.why, tw, lines)) {
            dl->AddText(bf, bf->FontSize, ImVec2(tx, y), ui::TEXT2, ln.c_str());
            y += lh;
        }
    }

    /* status + action, bottom right */
    const float bh = sp(4.f);
    const float by = o.y + inner_h - bh;
    float frac = -1.f;
    std::string lab;
    int run = 0;
    std::string gid;
    if (m.st == discover::Status::InPack) {
        gid = hubui::fg::identity(m.pack_sys, "", m.pack_title);   // the same id the library's pack ghost uses, so both show one progress
        run = gamepack_state_for(gid, &frac, &lab);
    }
    ImGui::SetCursorScreenPos(ImVec2(tx, by));
    if (m.st == discover::Status::Owned) {
        const float bw = sp(11.f);
        ui::pill_at(dl, ImVec2(tx, by + (bh - ui::F.cap->FontSize - sp(1.f)) * 0.5f), "In your library", mix(ui::GOOD, IM_COL32_BLACK, 0.2f));
        ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - bw, by));
        if (button("Play", ImVec2(bw, bh), true, true, ui::Icon::Play)) {
            if (const hub::GameEntry* e = find_entry(m.key)) { hub::GameEntry copy = *e; A.sel_game = e->key; play_entry_anim(copy); }
        }
    } else if (m.st == discover::Status::InPack) {
        const float bw = sp(11.f);
        if (run) {
            const float pw = tw - bw - sp(1.f);
            ImVec2 b0(tx, by + bh * 0.5f - sp(0.4f)), b1(tx + pw, by + bh * 0.5f + sp(0.4f));
            dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 255, 40), sp(0.4f));
            if (frac > 0) dl->AddRectFilled(b0, ImVec2(b0.x + pw * std::min(1.f, frac), b1.y), ui::ACCENT, sp(0.4f));
            dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, b0.y - ui::F.cap->FontSize - sp(0.5f)), ui::TEXT2, lab.c_str());
            ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - bw, by));
            if (button("Cancel", ImVec2(bw, bh), false, true, ui::Icon::Close)) gamepack_cancel(gid);
        } else {
            ui::pill_at(dl, ImVec2(tx, by + (bh - ui::F.cap->FontSize - sp(1.f)) * 0.5f), "In a shared pack", mix(ui::ACCENT, IM_COL32_BLACK, 0.2f));
            ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - bw, by));
            if (button("Install", ImVec2(bw, bh), true, true, ui::Icon::Cloud)) gamepack_enqueue(m.pack_sys, m.pack_title, gid);
        }
    } else {
        const float bw = sp(15.5f);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, by + (bh - ui::F.cap->FontSize) * 0.5f), ui::TEXT3,
                    ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, "Add your own copy to play", tw - bw - sp(1.f)).c_str());
        ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - bw, by));
        if (button("Add games...", ImVec2(bw, bh), false, true, ui::Icon::Plus)) add_games_open();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

}  // namespace

void discover_shot_console(const std::string& sys) { g_sys = sys; }

void page_discover()
{
    const std::vector<discover::Rec>& recs = discover::all();
    page_header("Discover", "Highly rated games for each console. Add your own copy, or install from your shared packs.");
    if (recs.empty()) {
        text_wrapped_disabled("The recommendations list (data\\recommended.json) is missing from this install.");
        return;
    }
    if (g_headless && std::getenv("HUB_SHOT_DISCOVER_FRIENDS")) g_friends = true;   // screenshots
    if (g_sys.empty()) {   // first visit: the console you own the most games for
        std::map<std::string, int> n;
        for (const auto& e : A.lib) ++n[sys_short(e)];
        int best = 0;
        g_sys = "n64";
        for (const std::string& c : discover::consoles())
            if (n[c] > best) { best = n[c]; g_sys = c; }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(1.f), sp(1.f)));
    {   // console picker (wraps) + the friends filter
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        bool first = true;
        auto place = [&](const char* lab) {
            const float w = ImGui::CalcTextSize(lab).x + sp(3.25f);
            if (!first && ImGui::GetItemRectMax().x + sp(1.f) + w <= right) ImGui::SameLine();
            first = false;
        };
        for (const std::string& c : discover::consoles()) {
            int n = (int)discover::for_console(recs, c, false).size();
            if (!n) continue;
            char lab[48];
            std::snprintf(lab, sizeof lab, "%s  %d", short_label(c), n);
            place(lab);
            ImU32 a = ui::ACCENT, b2;
            system_colors(hub::system_from_short(c), &a, &b2);
            if (chip(lab, g_sys == c, a)) g_sys = c;
        }
        place("Great with friends");
        if (chip("Great with friends", g_friends, ui::GOOD)) g_friends = !g_friends;
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    refresh_rows(recs);

    if (g_rows.empty()) {
        text_wrapped_disabled("No recommendations match this filter.");
    } else {
        const float gap = sp(1.5f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, (int)((avail + gap) / (sp(46.f) + gap)));
        const float cw = (avail - gap * (float)(cols - 1)) / (float)cols;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (size_t i = 0; i < g_rows.size(); ++i) {
            card(g_rows[i], (int)i, ImVec2(cw, sp(19.5f)));
            if ((int)(i + 1) % cols != 0 && i + 1 < g_rows.size()) ImGui::SameLine();
        }
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, sp(2.f)));
}
