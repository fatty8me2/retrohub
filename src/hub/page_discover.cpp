#include "app.h"
#include "art.h"
#include "discover_logic.h"
#include "gameinfo.h"
#include "gamepack.h"

#include <algorithm>
#include <sstream>

/* Discover: curated, highly rated games per console (data/recommended.json) and, behind "Full catalog", every other game the
 * offline database (data/gameinfo.tsv) knows. Each card says where the game stands:
 *   in your library -> Play;  in a shared pack -> Install (the GamePack queue, the only install path);
 *   otherwise -> "Add your own copy to play" and the normal Add games dialog. Nothing here downloads or links to game files.
 * Cover art for games you do not own is resolved the same way as for pack ghosts (art_for_ghost) and only for visible cards.
 * Sorting, filtering, paging and the catalog merge are pure code in discover_logic.h / discover_browse.cpp. */

namespace {

discover::Filter g_f;              // sys "" = all consoles
bool g_inited = false;             // console picked (first visit: the console you own the most games for)
discover::Pager g_pager;
discover::Browser g_br;
bool g_br_ready = false;
bool g_catalog_ok = false;         // gameinfo.tsv is there and was merged
discover::Browser::Result g_res;
std::string g_sig_f, g_sig_src;
int g_last_auto = -100;            // frame of the last scroll-triggered "show more"
char g_q[64] = {0};
bool g_shot_bottom = false;       // screenshots: scroll to the end of the page
std::string g_open_req;            // screenshots: popup to open on the first frame
int g_cat_count = -1;              // catalog games for the current filters (end-of-list offer); -1 = not worked out
std::string g_cat_sig;

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

std::string with_commas(int n)
{
    std::string s = std::to_string(n), o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return o;
}

/* Screenshots (HUB_SHOT_DISCOVER_FAKE=1, headless): a made-up library that owns every 5th game and a made-up shared pack that holds
 * the 2nd and 4th of each five, so all three card states show. `list` is the curated list, and (once merged) the catalog.
 * Never touches files or the network. */
std::vector<hubgp::PackGame> g_fake_pack;
int g_fake_seeded_n = -1;
void shot_seed()
{
    if (!g_headless || !std::getenv("HUB_SHOT_DISCOVER_FAKE")) return;
    const int n = (int)g_br.size();
    if (g_fake_seeded_n == n) return;
    g_fake_seeded_n = n;
    for (size_t i = A.lib.size(); i-- > 0;) if (A.lib[i].key.rfind("fake:disc:", 0) == 0) A.lib.erase(A.lib.begin() + (long)i);
    g_fake_pack.clear();
    std::map<std::string, int> per;
    for (int id = 0; id < n; ++id) {
        const discover::Rec& r = g_br.item(id);
        int i = per[r.sys + (r.curated ? "" : "!")]++;
        const int mod = r.curated ? 5 : 25;   // the 11,000-game catalog gets a thinner made-up library
        if (i % mod == 0) {
            hub::GameEntry g;
            g.system = hub::system_from_short(r.sys);
            g.key = "fake:disc:" + std::to_string(id);
            g.title = r.title;
            g.catalog_id = r.cid;
            g.path = "C:/fake/" + g.key + ".rom";
            A.lib.push_back(g);
        } else if (i % mod == 1 || i % mod == 3) {
            g_fake_pack.push_back({r.sys, r.title, hubgp::pack_key(r.title), r.title + " (USA)"});
        }
    }
    hubgp::index_test_set(g_fake_pack);
}

/* Screenshots (HUB_SHOT_DISCOVER_BIG=1): ~100 made-up "recommended" games per console built from the real database, so the page is
 * seen with the ~1,300 entries it is designed for. */
std::vector<discover::Rec> big_fake_list(const std::vector<discover::Rec>& real)
{
    std::vector<discover::Rec> out;
    const gameinfo::Db& db = gameinfo::shared_db(hub::exe_dir() + "data/");
    static const char* kWhy[] = {"A genuine classic that still plays beautifully today.", "Tight controls, great level design and a catchy soundtrack.",
                                 "Easy to pick up with friends and hard to put down.", "One of the best of its generation: deep, polished and full of surprises."};
    for (const std::string& c : discover::consoles()) {
        int n = 0;
        for (const discover::Rec& r : real) if (r.sys == c) { out.push_back(r); ++n; }
        for (const gameinfo::Db::Row& row : db.rows()) {
            if (n >= 100) break;
            if (row.sys != c || row.year < 1980 || row.year > 2020) continue;
            discover::Rec r;
            r.title = row.title;
            r.sys = c;
            r.genre = discover::genre_bucket(row.genre);
            if (r.genre == "Other" || r.genre == "Educational" || r.genre == "Compilation" || r.genre == "Board & Card") r.genre = "Action";
            r.year = row.year;
            r.players = row.players > 0 ? row.players : 1 + (n % 4);
            r.friends = r.players >= 2 && n % 3 == 0;
            r.why = kWhy[n % 4];
            out.push_back(r);
            ++n;
        }
    }
    return out;
}

void ensure_browser()
{
    if (g_br_ready) return;
    g_br_ready = true;
    std::vector<discover::Rec> list = discover::all();
    if (g_headless && std::getenv("HUB_SHOT_DISCOVER_BIG")) list = big_fake_list(list);
    g_br.set_curated(list);
    const gameinfo::Db& db = gameinfo::shared_db(hub::exe_dir() + "data/");
    if (db.loaded()) {
        g_br.set_catalog(discover::catalog_from(db, list));
        g_catalog_ok = g_br.has_catalog();
    }
    g_f.sort = discover::sort_from_key(A.st.discover_sort);
    if (g_headless) {   // HUB_SHOT_DISCOVER_VIEW="catalog;sort=new;genre=RPG|Action;players=2;decade=1990;status=missing;q=mario;more=1;friends;open=genre"
        if (const char* v = std::getenv("HUB_SHOT_DISCOVER_VIEW")) {
            std::stringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ';')) {
                std::string k = tok, val;
                size_t eq = tok.find('=');
                if (eq != std::string::npos) { k = tok.substr(0, eq); val = tok.substr(eq + 1); }
                if (k == "catalog") g_f.catalog = g_catalog_ok;
                else if (k == "sort") g_f.sort = discover::sort_from_key(val);
                else if (k == "genre") { std::stringstream gs(val); std::string g; while (std::getline(gs, g, '|')) g_f.genres.insert(g); }
                else if (k == "players") g_f.min_players = std::atoi(val.c_str());
                else if (k == "decade") g_f.decade = std::atoi(val.c_str());
                else if (k == "friends") g_f.friends = true;
                else if (k == "status") g_f.status = val == "owned" ? discover::StatusFilter::Owned : val == "pack" ? discover::StatusFilter::InPack : discover::StatusFilter::NotOwned;
                else if (k == "q") { std::snprintf(g_q, sizeof g_q, "%s", val.c_str()); g_f.query = g_q; }
                else if (k == "more") g_pager.shown += discover::kBatch * (size_t)std::atoi(val.c_str());
                else if (k == "open") g_open_req = val;
                else if (k == "bottom") g_shot_bottom = true;
            }
        }
    }
}

void sync_results()
{
    shot_seed();
    char sig[160];
    std::snprintf(sig, sizeof sig, "%zu|%u|%zu", A.lib.size(), hubgp::index_epoch(), g_br.size());
    if (g_sig_src != sig) {
        g_sig_src = sig;
        g_br.set_sources(owned_snapshot(), hubgp::index_games());
        g_sig_f.clear();
        g_cat_sig.clear();
    }
    const std::string fsig = discover::filter_sig(g_f);
    if (fsig != g_sig_f || g_res.pending > 0) {
        const bool changed = fsig != g_sig_f, had = !g_sig_f.empty();
        g_res = g_br.query(g_f, 5.0);
        if (changed && had) g_pager.reset();
        g_sig_f = fsig;
    }
}

/* ---------------------------------------------------------------- the card */

void card(int id, ImVec2 size)
{
    const discover::Rec& r = g_br.item(id);
    const discover::Match m = g_br.match(id);
    const GameInfo* cat = r.cid.empty() ? nullptr : hub::find_game(r.cid);
    hub::System sy = hub::system_from_short(r.sys);
    ImU32 c0, c1;
    system_colors(sy, &c0, &c1);

    ImGui::PushID(id);
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

    /* cover: the owned copy's art, else the ghost lookup (catalog name / DAT title / fuzzy title), else the no-art placeholder.
     * Only cards on screen look anything up. */
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
        std::string meta;
        auto add = [&](const std::string& s) { if (!s.empty()) { if (!meta.empty()) meta += "  -  "; meta += s; } };
        if (g_f.sys.empty()) add(short_label(r.sys));   // all-consoles view: say which console
        if (r.year > 0) add(std::to_string(r.year));
        if (r.genre != "Other") add(r.genre);
        if (r.players > 0) add(discover::players_text(r.players));
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
    {   // why it is great (catalog games: who made it)
        ImFont* bf = ui::F.cap;
        const float lh = bf->FontSize * 1.25f;
        const float footer_h = sp(5.25f);
        int lines = (int)std::max(1.f, std::min(3.f, (o.y + inner_h - footer_h - y) / lh));
        std::string body = r.why;
        ImU32 col = ui::TEXT2;
        if (r.curated) {}
        else {
            body = r.dev;
            if (!r.pub.empty() && r.pub != r.dev) body += body.empty() ? r.pub : "  /  " + r.pub;
            if (body.empty()) body = "No more details on file.";
            if (r.score > 0) body += "  -  Rated " + std::to_string(r.score) + "/100";
            col = ui::TEXT3;
        }
        for (const std::string& ln : wrap_lines(bf, bf->FontSize, body, tw, lines)) {
            dl->AddText(bf, bf->FontSize, ImVec2(tx, y), col, ln.c_str());
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

/* ---------------------------------------------------------------- toolbar */

/* Wrapping flow: puts the next element of width w on this line when it fits, else on a new one. */
struct Flow {
    float right = 0;
    float end_x = 0;   // right edge of the last element
    bool first = true;
    void place(float w)
    {
        if (!first && end_x + sp(1.f) + w <= right) ImGui::SameLine();
        first = false;
    }
    void mark() { end_x = ImGui::GetItemRectMax().x; }
};

float chip_w(const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x + sp(3.25f); }

/* A chip that opens a popup menu under itself; returns true while the popup is open (call end_menu after the items). */
bool menu_begin(const char* id, const std::string& label, bool active, ImU32 col, Flow& fl, float min_w = 16.f)
{
    fl.place(chip_w(label));
    ImGui::PushID(id);
    const bool pressed = chip(label.c_str(), active, col);
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    fl.mark();
    if (pressed || g_open_req == id) { ImGui::OpenPopup("menu"); g_open_req.clear(); }
    ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + sp(0.5f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(sp(min_w), 0), ImVec2(sp(40.f), sp(60.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(0.75f), sp(0.75f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(1.f), sp(0.25f)));
    if (ImGui::BeginPopup("menu")) return true;
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return false;
}
void menu_end()
{
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
}
bool menu_item(const std::string& label, bool selected, bool keep_open = false)
{
    return ImGui::Selectable(label.c_str(), selected, keep_open ? ImGuiSelectableFlags_NoAutoClosePopups : 0, ImVec2(0, ImGui::GetFontSize() + sp(0.75f)));
}

/* The live count ("100 games"), right-aligned on the line of the last element when it fits. Returns false when it did not fit. */
bool count_text(Flow& f, bool force)
{
    char t[64];
    if (g_res.pending > 0) std::snprintf(t, sizeof t, "Checking your library...");
    else std::snprintf(t, sizeof t, "%s %s", with_commas((int)g_res.ids.size()).c_str(), g_res.ids.size() == 1 ? "game" : "games");
    const float w = ImGui::CalcTextSize(t).x;
    if (f.end_x + sp(2.f) + w <= f.right) ImGui::SameLine(0, 0);
    else if (force) { ImGui::NewLine(); ImGui::SameLine(0, 0); }
    else return false;
    const float cy = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(f.right - w, cy + sp(0.6f)));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    return true;
}

void toolbar()
{
    Flow fl;
    fl.right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

    /* row 1: the console picker, counts follow the filters */
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    {
        char lab[48];
        std::snprintf(lab, sizeof lab, "All  %d", g_res.total_all);
        fl.place(chip_w(lab));
        ImGui::PushID("all");
        if (chip(lab, g_f.sys.empty(), ui::ACCENT)) g_f.sys.clear();
        fl.mark();
        ImGui::PopID();
        for (const std::string& c : discover::consoles()) {
            if (g_f.catalog && !discover::catalog_covers(c)) continue;
            auto it = g_res.by_console.find(c);
            int n = it == g_res.by_console.end() ? 0 : it->second;
            if (!n && g_f.sys != c) continue;
            std::snprintf(lab, sizeof lab, "%s  %d", short_label(c), n);
            fl.place(chip_w(lab));
            ImU32 a = ui::ACCENT, b2;
            system_colors(hub::system_from_short(c), &a, &b2);
            ImGui::PushID(c.c_str());
            if (chip(lab, g_f.sys == c, a)) g_f.sys = c;
            fl.mark();
            ImGui::PopID();
        }
    }
    ImGui::PopItemFlag();
    const bool counted = count_text(fl, false);   // the console row's last line usually has room
    if (!counted) ImGui::Dummy(ImVec2(0, sp(0.25f)));

    /* row 2: search, sort and filters in one slim row (wraps on a narrow window) */
    Flow f2;
    f2.right = fl.right;
    {
        const float bh = sp(3.75f);
        const float w = std::min(sp(21.f), ImGui::GetContentRegionAvail().x);
        f2.place(w);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sp(4.f), (bh - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
        ImGui::SetNextItemWidth(w);
        osk::input("##dsearch", g_f.catalog ? "Search title or developer" : "Search title", g_q, sizeof g_q);
        ImVec2 s0 = ImGui::GetItemRectMin(), s1 = ImGui::GetItemRectMax();
        f2.mark();
        ImGui::GetWindowDrawList()->AddRect(s0, s1, ImGui::IsItemActive() ? ui::ACCENT : ui::HAIRLINE, bh * 0.5f, 0, 1.f);
        ui::icon(ImGui::GetWindowDrawList(), ui::Icon::Search, ImVec2(s0.x + sp(2.f), (s0.y + s1.y) * 0.5f), sp(1.75f), ui::TEXT2);
        ImGui::PopStyleVar(2);
        g_f.query = g_q;
    }
    {   // Sort
        const std::string lab = std::string("Sort: ") + discover::sort_label(g_f.sort);
        if (menu_begin("sort", lab, g_f.sort != discover::Sort::Recommended, ui::ACCENT, f2, 17.f)) {
            for (int i = 0; i < discover::kSortCount; ++i) {
                discover::Sort s = (discover::Sort)i;
                if (menu_item(discover::sort_label(s), g_f.sort == s)) {
                    g_f.sort = s;
                    A.st.discover_sort = discover::sort_key(s);
                    mark_dirty();
                }
            }
            menu_end();
        }
    }
    {   // Genre (multi-select)
        std::string lab = "Genre";
        if (g_f.genres.size() == 1) lab += ": " + *g_f.genres.begin();
        else if (g_f.genres.size() > 1) lab += ": " + std::to_string(g_f.genres.size()) + " picked";
        if (menu_begin("genre", lab, !g_f.genres.empty(), ui::ACCENT, f2, 18.f)) {
            if (menu_item("Any genre", g_f.genres.empty())) g_f.genres.clear();
            std::vector<std::pair<std::string, int>> gc = g_br.genre_counts(g_f);
            for (const std::string& s : g_f.genres) {   // a picked genre with no match under the other filters stays listed so it can be unpicked
                bool have = false;
                for (auto& kv : gc) if (kv.first == s) have = true;
                if (!have) gc.push_back({s, 0});
            }
            std::sort(gc.begin(), gc.end());
            for (auto& kv : gc) {
                const bool on = g_f.genres.count(kv.first) > 0;
                char row[96];
                std::snprintf(row, sizeof row, "%s%s  (%d)", on ? "[x] " : "[  ] ", kv.first.c_str(), kv.second);
                if (menu_item(row, on, true)) { if (on) g_f.genres.erase(kv.first); else g_f.genres.insert(kv.first); }
            }
            menu_end();
        }
    }
    {   // Players
        static const char* names[] = {"Any number", "2 or more", "3 or more", "4 or more"};
        static const int vals[] = {0, 2, 3, 4};
        std::string lab = "Players";
        for (int i = 1; i < 4; ++i) if (g_f.min_players == vals[i]) lab = "Players: " + std::to_string(vals[i]) + "+";
        if (menu_begin("players", lab, g_f.min_players > 0, ui::ACCENT, f2, 14.f)) {
            for (int i = 0; i < 4; ++i) if (menu_item(names[i], g_f.min_players == vals[i])) g_f.min_players = vals[i];
            menu_end();
        }
    }
    {   // Decade
        std::string lab = g_f.decade ? "Decade: " + std::to_string(g_f.decade) + "s" : "Decade";
        if (menu_begin("decade", lab, g_f.decade > 0, ui::ACCENT, f2, 13.f)) {
            if (menu_item("Any decade", g_f.decade == 0)) g_f.decade = 0;
            for (int d = 1980; d <= 2010; d += 10) if (menu_item(std::to_string(d) + "s", g_f.decade == d)) g_f.decade = d;
            menu_end();
        }
    }
    {   // Status
        static const char* names[] = {"Any status", "Not in my library", "Installable from a shared pack", "In my library"};
        static const char* shorts[] = {"Status", "Status: Not mine", "Status: In a pack", "Status: Mine"};
        const int cur = (int)g_f.status;
        if (menu_begin("status", shorts[cur], cur != 0, ui::ACCENT, f2, 18.f)) {
            for (int i = 0; i < 4; ++i) if (menu_item(names[i], cur == i)) g_f.status = (discover::StatusFilter)i;
            menu_end();
        }
    }
    if (!g_f.catalog) {
        f2.place(chip_w("Friends"));
        ImGui::PushID("friends");
        if (chip("Friends", g_f.friends, ui::GOOD)) g_f.friends = !g_f.friends;
        f2.mark();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only games that are great with friends");
        ImGui::PopID();
    }
    const bool cat_here = g_catalog_ok && (g_f.sys.empty() || discover::catalog_covers(g_f.sys));
    if (g_catalog_ok && (cat_here || g_f.catalog)) {
        const char* lab = "Full catalog";
        f2.place(chip_w(lab));
        ImGui::PushID("catalog");
        if (chip(lab, g_f.catalog, ui::ACCENT)) {
            g_f.catalog = !g_f.catalog;
            if (g_f.catalog && !g_f.sys.empty() && !discover::catalog_covers(g_f.sys)) g_f.sys.clear();
        }
        f2.mark();
        ImGui::PopID();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every other game in the offline database, not just the recommended ones");
    }
    if (discover::filters_active(g_f)) {
        f2.place(chip_w("Clear filters"));
        ImGui::PushID("clear");
        if (chip("Clear filters", false, ui::ACCENT)) { discover::clear_filters(&g_f); g_q[0] = 0; }
        f2.mark();
        ImGui::PopID();
    }
    if (!counted) count_text(f2, true);
}

}  // namespace

void discover_shot_console(const std::string& sys) { g_f.sys = sys == "all" ? std::string() : sys; g_inited = true; }

void page_discover()
{
    page_header("Discover", "Highly rated games for each console. Add your own copy, or install from your shared packs.");
    ensure_browser();
    if (g_br.size() == 0) {
        text_wrapped_disabled("The recommendations list (data\\recommended.json) is missing from this install.");
        return;
    }
    if (!g_inited) {   // first visit: the console you own the most games for
        g_inited = true;
        std::map<std::string, int> n;
        for (const auto& e : A.lib) ++n[sys_short(e)];
        int best = 0;
        g_f.sys = "n64";
        for (const std::string& c : discover::consoles())
            if (n[c] > best) { best = n[c]; g_f.sys = c; }
    }

    sync_results();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(1.f), sp(1.f)));
    toolbar();
    ImGui::PopStyleVar();
    sync_results();   // a toolbar change shows in this same frame
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    const size_t total = g_res.ids.size();
    if (total == 0 && g_res.pending == 0) {
        ImGui::Dummy(ImVec2(0, sp(2.f)));
        const float cx = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        ui::icon(dl, ui::Icon::Search, ImVec2(cx, o.y + sp(2.5f)), sp(4.f), ui::TEXT3);
        auto centered = [&](ImFont* f, float y, ImU32 col, const char* t) {
            dl->AddText(f, f->FontSize, ImVec2(cx - ImGui::CalcTextSize(t).x * 0.5f, y), col, t);
        };
        centered(ui::F.h2, o.y + sp(6.f), ui::TEXT, "No games match these filters");
        centered(ui::F.body, o.y + sp(6.f) + ui::F.h2->FontSize + sp(0.75f), ui::TEXT2,
                 g_f.catalog ? "Try fewer filters or a different spelling." : "Try fewer filters, or look through the full catalog.");
        ImGui::Dummy(ImVec2(0, sp(11.f)));
        const float bw = sp(16.f), bw2 = sp(22.f);
        const bool can_cat = g_catalog_ok && !g_f.catalog && (g_f.sys.empty() || discover::catalog_covers(g_f.sys));
        const float both = discover::filters_active(g_f) ? (can_cat ? bw + bw2 + sp(1.f) : bw) : (can_cat ? bw2 : 0.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - both) * 0.5f);
        if (discover::filters_active(g_f)) {
            if (button("Clear filters", ImVec2(bw, sp(4.5f)), !can_cat, true, ui::Icon::Close)) { discover::clear_filters(&g_f); g_q[0] = 0; }
            if (can_cat) ImGui::SameLine(0, sp(1.f));
        }
        if (can_cat && button("Browse the full catalog", ImVec2(bw2, sp(4.5f)), true, true, ui::Icon::Search)) g_f.catalog = true;
        ImGui::Dummy(ImVec2(0, sp(2.f)));
        return;
    }

    const float gap = sp(1.5f);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, (int)((avail + gap) / (sp(46.f) + gap)));
    const float cw = (avail - gap * (float)(cols - 1)) / (float)cols;
    const float ch = sp(19.5f);
    const size_t shown = g_pager.visible(total);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    {   // only the rows on screen are drawn (a long list stays cheap)
        const int rows = (int)((shown + (size_t)cols - 1) / (size_t)cols);
        ImGuiListClipper clip;
        clip.Begin(rows, ch + gap);
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                for (int c = 0; c < cols; ++c) {
                    const size_t i = (size_t)row * (size_t)cols + (size_t)c;
                    if (i >= shown) break;
                    if (c) ImGui::SameLine();
                    card(g_res.ids[i], ImVec2(cw, ch));
                }
            }
        }
    }
    ImGui::PopStyleVar();

    /* below the cards: "Showing 24 of 103" + Show more, or the offer of the full catalog when the curated list has run out */
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    {
        const std::string st = discover::showing_text(shown, total);
        const float bh = sp(4.5f), bw = sp(18.f);
        if (g_pager.has_more(total)) {
            const float tw = ImGui::CalcTextSize(st.c_str()).x;
            const float group = tw + sp(2.f) + bw;
            const float x0 = ImGui::GetCursorPosX() + std::max(0.f, (ImGui::GetContentRegionAvail().x - group) * 0.5f);
            const float y0 = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(x0, y0 + (bh - ImGui::GetFontSize()) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            ImGui::TextUnformatted(st.c_str());
            ImGui::PopStyleColor();
            ImGui::SetCursorPos(ImVec2(x0 + tw + sp(2.f), y0));
            const size_t next = std::min(total - shown, discover::kBatch);
            char bl[48];
            std::snprintf(bl, sizeof bl, "Show %zu more", next);
            ImGui::PushID("more");
            const bool pressed = button(bl, ImVec2(bw, bh), true, true, ui::Icon::ChevronR);
            ImGui::PopID();
            const bool vis = ImGui::IsItemVisible(), foc = ImGui::IsItemFocused();
            const int fr = ImGui::GetFrameCount();
            if (pressed) g_pager.more(total);
            else if (!g_shot_bottom && vis && (ImGui::GetScrollY() > 1.f || foc) && fr - g_last_auto > 8) {   // scrolled / navigated to the end: load the next batch
                g_pager.more(total);
                g_last_auto = fr;
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            const float tw = ImGui::CalcTextSize(st.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - tw) * 0.5f);
            ImGui::TextUnformatted(st.c_str());
            ImGui::PopStyleColor();
            const bool offer = !g_f.catalog && g_catalog_ok && (g_f.sys.empty() || discover::catalog_covers(g_f.sys));
            if (offer) {
                discover::Filter cf = g_f;
                cf.catalog = true;
                cf.status = discover::StatusFilter::Any;
                const std::string csig = discover::filter_sig(cf) + g_sig_src;
                if (csig != g_cat_sig) { g_cat_sig = csig; g_cat_count = (int)g_br.query(cf).ids.size(); }
                ImGui::Dummy(ImVec2(0, sp(0.5f)));
                const std::string where = g_f.sys.empty() ? std::string("") : std::string(short_label(g_f.sys)) + " ";
                std::string head = "That is every pick here. Want more?";
                const std::string btn = "Browse the full " + where + "catalog" + (g_cat_count > 0 && g_f.status == discover::StatusFilter::Any ? "  (" + with_commas(g_cat_count) + " more)" : "");
                const float bw2 = ImGui::CalcTextSize(btn.c_str()).x + sp(8.f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(head.c_str()).x) * 0.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
                ImGui::TextUnformatted(head.c_str());
                ImGui::PopStyleColor();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw2) * 0.5f);
                ImGui::PushID("browse_catalog");
                if (button(btn.c_str(), ImVec2(bw2, bh), true, true, ui::Icon::Search)) {
                    g_f.catalog = true;
                    g_f.status = discover::StatusFilter::Any;
                    g_pager.reset();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::Dummy(ImVec2(0, sp(2.f)));
    if (g_shot_bottom && g_headless && ImGui::GetFrameCount() > 20 && ImGui::GetFrameCount() < 60) ImGui::SetScrollHereY(1.f);
}
