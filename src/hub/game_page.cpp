/* The game page as host-independent pieces (see game_page.h). game_page_view.cpp lays them out inside a ui::sheet for both hosts. */
#include "app.h"
#include "game_page.h"
#include "game_page_layout.h"
#include "winutil.h"
#include "launch_guard_logic.h"
#include "wave3_hooks.h"
#include "gamepack.h"
#include "wiiu_meta.h"
#include "hack_ui.h"
#include "hookshot_ui.h"
#include "hidden_ui.h"
#include "playtime_logic.h"
#include "option_sections.h"
#include "page_cache.h"
#include "qa_diskio.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

/* gameinfo_ui.cpp: the info block; scroll_hold nullptr = never touch the window's scroll (the page body opens at the top anyway) */
bool gameinfo_ui_section_held(const hub::GameEntry& e, const std::string& catalog_title, int part, double* scroll_hold);

namespace {

/* ---------- Wii U details: updates / DLC, Cemu game profile, save accounts, key help ---------- */

/* What the Wii U details need from the disk: the info lines, plus the Cemu install and title id that player 1's controller is decided from
 * (cemu_player1_for, the same call the launch makes; the game's settings are applied per frame, so a changed setting shows at once). */
struct WiiuFacts {
    std::vector<std::string> lines;
    hubemu::Install inst;
    uint64_t tid = 0;
    bool ready = false;   // false until the first scan for this game landed
};

/* Disk-heavy (Cemu install probe, WUA directory, mlc01 save accounts): runs on a worker thread. */
WiiuFacts wiiu_info_compute(const hub::GameEntry& e, const std::string& cemu_exe) {
    WiiuFacts out;
    std::vector<std::string>& s_lines = out.lines;
    hubemu::Install inst = hubemu::find(hubemu::Kind::Cemu, cemu_exe, e.path);
    std::string game = launch_path_for(e);
    if (e.issue == "extract" && game == e.path) game.clear();   // still packed: nothing to read yet
    const uint64_t tid = hubemu::wiiu_title_id_for(e, game);   // as the launch works it out
    out.inst = inst;
    out.tid = tid;
    out.ready = true;
    hub::FsProbe fp = hubemu::wiiu_probe();
    std::string user_data, mlc;
    if (!inst.exe.empty()) {
        if (inst.managed) { user_data = inst.dir + "\\portable"; mlc = user_data + "\\mlc01"; }
        else {
            const std::string ad_s = winutil::getenv_utf8("APPDATA"); const char* ad = ad_s.empty() ? nullptr : ad_s.c_str();
            hub::CemuDirs d = hub::cemu_dirs_for(inst.dir, ad ? ad : "", fp);
            user_data = d.user_data;
            mlc = d.mlc;
        }
    }
    if (tid) {
        hub::WiiuAddons a = hub::wiiu_installed_addons(tid, mlc, fp);
        if (!game.empty() && game.size() > 4) {
            std::ifstream f(std::filesystem::u8path(game), std::ios::binary);
            std::error_code ec;
            uint64_t sz = std::filesystem::file_size(std::filesystem::u8path(game), ec);
            if (f && !ec) {
                hub::ReadAtFn rd = [&](uint64_t off, void* buf, size_t n) { f.clear(); f.seekg((std::streamoff)off); f.read((char*)buf, (std::streamsize)n); return f.gcount() == (std::streamsize)n; };
                hub::wiiu_addons_from_wua(hub::wua_titles(rd, sz), &a);
            }
        }
        s_lines.push_back("Updates and DLC: " + hub::wiiu_addons_text(a));
        if (!mlc.empty()) {
            auto rels = hub::wiiu_save_rels(tid);
            std::vector<std::string> acc = hub::wiiu_save_accounts(mlc + "/" + rels[0]);
            if (!acc.empty()) { std::string t = "Cemu save accounts:"; for (auto& x : acc) t += " " + x; s_lines.push_back(t); }
        }
        std::string prof = hubemu::cemu_profile_summary(inst.dir, user_data, tid);
        if (!prof.empty()) s_lines.push_back("Cemu profile: " + prof);
    }
    return out;
}

/* UI thread: returns the last finished result for this game (empty until the first scan lands). A scan runs in the background when
 * pagecache::need() asks for one: the full-window page re-scans every 4 s as it always did; the desk pane once the cursor has settled on
 * the game (and again after pagecache::invalidate()), never while a game runs. A per-game disk cache shared by every host (its worker
 * outlives any one page), so it is not part of gamepage::State. Screenshot runs scan on the UI thread at the same moments (deterministic). */
WiiuFacts wiiu_info_lines(const hub::GameEntry& e) {
    struct Bg { std::mutex mu; bool running = false; std::string key; WiiuFacts lines; };
    static Bg* bg = new Bg();
    static std::string s_key;   // the game the last scan was started for, when, under which pagecache generation
    static double s_at = -100;
    static unsigned s_gen = 0;
    static WiiuFacts s_lines;
    pagecache::Need n = pagecache::Need::Use;
    {
        std::lock_guard<std::mutex> l(bg->mu);
        if (bg->key == e.key) s_lines = bg->lines;   // only ever show a result computed for THIS game
        else s_lines = WiiuFacts();
        n = pagecache::need(s_key == e.key, s_gen, s_at, 4.0);
        if (n == pagecache::Need::Use) return s_lines;
        if (bg->running) return s_lines;   // A's scan still running: B's starts on the first frame after it lands
        bg->running = true;
    }
    s_key = e.key;
    s_at = ImGui::GetTime();
    s_gen = pagecache::generation();
    qadisk::note_worker("wiiu-info");
    if (g_headless) {   // screenshots stay deterministic: the same scan, on this thread
        qadisk::Scope qs(pagecache::scope_name(n), false);
        WiiuFacts r = wiiu_info_compute(e, A.st.cemu_exe);
        std::lock_guard<std::mutex> l(bg->mu);
        bg->key = e.key;
        bg->lines = r;
        bg->running = false;
        return s_lines = r;
    }
    std::thread([e, cemu = A.st.cemu_exe] {
        if (g_io_shutdown) return;
        WiiuFacts r = wiiu_info_compute(e, cemu);
        if (g_io_shutdown) return;
        std::lock_guard<std::mutex> l(bg->mu);
        bg->key = e.key;
        bg->lines = std::move(r);
        bg->running = false;
    }).detach();
    return s_lines;
}

/* ---------- play time (Steam-style header line + stats strip) ---------- */

/* local time minus UTC in seconds (same recipe as wrapped_ui.cpp) */
int pt_local_tz() {
    std::time_t t = std::time(nullptr);
    std::tm lt{}, gm{};
    localtime_s(&lt, &t);
    gmtime_s(&gm, &t);
    int tz = (int)std::difftime(std::mktime(&lt), std::mktime(&gm));
    if (lt.tm_isdst > 0) tz += 3600;
    return tz;
}

/* Screenshot aid (HUB_SHOT_PLAYTIME=1, headless): deterministic sessions instead of the vault. This PC's sessions add up to exactly
 * the seeded total (lib_state.cpp), the newest starts at the seeded last-played time. */
std::vector<hub::saves::Session> pt_fake_sessions(const std::string& key, const hub::GameState& gs) {
    std::vector<hub::saves::Session> v;
    if (!gs.play_seconds || !gs.last_played) return v;
    const int n = key == "fake:1" ? 30 : key == "fake:0" ? 8 : 3;
    const int64_t T = (int64_t)gs.play_seconds, last = (int64_t)gs.last_played;
    const int64_t gap = key == "fake:1" ? (int64_t)(5.5 * 86400) : key == "fake:0" ? 129600 : 86400;
    int64_t left = T;
    for (int j = 0; j < n; ++j) {
        hub::saves::Session s;
        s.system = "fake";
        s.key = key;
        s.start = last - (int64_t)j * gap;
        s.secs = (j == n - 1) ? left : T / n;
        left -= s.secs;
        v.push_back(s);
    }
    if (key == "fake:0") {   // a second PC, about 3.1 hours
        hub::saves::Session a, b;
        a.system = b.system = "fake";
        a.key = b.key = key;
        a.device = b.device = "LAPTOP";
        a.start = (int64_t)gs.last_played - 20 * 86400; a.secs = 5400;
        b.start = (int64_t)gs.last_played - 30 * 86400; b.secs = 5760;
        v.push_back(a);
        v.push_back(b);
    }
    return v;
}

/* The shown game's numbers, cached in the page state (see PlayTimeCache). The full-window page reads the play log again every 30 s and
 * each time it opens; the desk pane once the cursor has settled on the game and after pagecache::invalidate(), never while a game runs
 * (pagecache::need). Until then: nothing (the strip waits), or the last numbers of this same game. */
const hubui::pt::GameStats& pt_stats_for(gamepage::PlayTimeCache& C, const hub::GameEntry& e, const hub::GameState& gs, int64_t now) {
    static const hubui::pt::GameStats none{};
    const int fr = ImGui::GetFrameCount();
    const bool same = C.ok && C.key == e.key && C.ps == gs.play_seconds && C.lp == gs.last_played;
    const bool reopened = fr - C.frame > 2 && pagecache::host() != pagecache::Host::Pane;   // the full-window page closed and opened again
    const pagecache::Need n = pagecache::need(same && !reopened, C.gen, C.at, 30.0);
    if (n != pagecache::Need::Use) {
        qadisk::Scope qs(pagecache::scope_name(n), false);
        static const bool fake_on = g_headless && std::getenv("HUB_SHOT_PLAYTIME") != nullptr;
        const bool fake = fake_on && e.key.compare(0, 5, "fake:") == 0;   // only the made-up games get made-up sessions
        std::vector<hub::saves::Session> sessions = fake ? pt_fake_sessions(e.key, gs) : hub::saves::playlog_read_game(sys_short(e), e.key);
        C.st = hubui::pt::game_stats(gs.play_seconds, gs.last_played, sessions, hub::saves::playlog_device(), now);
        C.key = e.key;
        C.ps = gs.play_seconds;
        C.lp = gs.last_played;
        C.at = ImGui::GetTime();
        C.gen = pagecache::generation();
        C.ok = true;
    }
    C.frame = fr;
    return C.ok && C.key == e.key ? C.st : none;
}

/* shorten text with "..." until it is at most w wide in the current font (last resort only; UTF-8 safe) */
std::string pt_fit(const std::string& s, float w) { return ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), s, w); }

/* The stats strip: one rounded panel, cells = small caption over a bold value, hairline separators; cells wrap greedily into rows
 * when they do not fit max_w. Draw-only (one Dummy reserves the space), no focusable items. */
void draw_playtime_strip(const std::vector<hubui::pt::Cell>& cells, float max_w) {
    if (cells.empty()) return;
    const float padx = sp(1.5f), pady = sp(0.9f), gap_y = sp(0.25f), rad = sp(1.25f);
    float cap_h = 0.f, val_h = 0.f;
    ImGui::PushFont(ui::F.cap); cap_h = ImGui::GetTextLineHeight(); ImGui::PopFont();
    ImGui::PushFont(ui::F.bold); val_h = ImGui::GetTextLineHeight(); ImGui::PopFont();
    struct Item { std::string label, cap, val; float w = 0.f; };
    std::vector<Item> items;
    bool has_other = false;
    for (const hubui::pt::Cell& c : cells) {
        Item it;
        it.label = c.label;
        it.cap = c.label;
        for (char& ch : it.cap) ch = (char)std::toupper((unsigned char)ch);
        it.val = c.value;
        const float inner = std::max(sp(4.f), max_w - 2.f * padx);
        ImGui::PushFont(ui::F.cap); it.cap = pt_fit(it.cap, inner); const float cw = ImGui::CalcTextSize(it.cap.c_str()).x; ImGui::PopFont();
        ImGui::PushFont(ui::F.bold); it.val = pt_fit(it.val, inner); const float vw = ImGui::CalcTextSize(it.val.c_str()).x; ImGui::PopFont();
        it.w = std::max(cw, vw) + 2.f * padx;
        if (c.label.compare(0, 8, "Other PC") == 0) has_other = true;
        items.push_back(it);
    }
    /* greedy rows */
    std::vector<std::vector<size_t>> rows(1);
    float used = 0.f, panel_w = 0.f;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!rows.back().empty() && used + items[i].w > max_w) { rows.emplace_back(); used = 0.f; }
        rows.back().push_back(i);
        used += items[i].w;
        panel_w = std::max(panel_w, used);
    }
    const float row_h = 2.f * pady + cap_h + gap_y + val_h;
    const float panel_h = row_h * (float)rows.size();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + panel_w, p0.y + panel_h);
    ImGui::Dummy(ImVec2(panel_w, panel_h));
    dl->AddRectFilled(p0, p1, ui::PANEL2, rad);
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);
    const bool hov_ok = ImGui::IsWindowHovered();
    for (size_t r = 0; r < rows.size(); ++r) {
        const float y = p0.y + row_h * (float)r;
        if (r > 0) dl->AddLine(ImVec2(p0.x + 1.f, y), ImVec2(p1.x - 1.f, y), ui::HAIRLINE, 1.f);
        float x = p0.x;
        for (size_t k = 0; k < rows[r].size(); ++k) {
            const Item& it = items[rows[r][k]];
            if (k > 0) dl->AddLine(ImVec2(x, y + pady * 0.7f), ImVec2(x, y + row_h - pady * 0.7f), ui::HAIRLINE, 1.f);
            ImGui::PushFont(ui::F.cap);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + padx, y + pady), ui::TEXT3, it.cap.c_str());
            ImGui::PopFont();
            ImGui::PushFont(ui::F.bold);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + padx, y + pady + cap_h + gap_y), ui::TEXT, it.val.c_str());
            ImGui::PopFont();
            if (has_other && hov_ok && it.label == "Play time" && ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + it.w, y + row_h)))
                ImGui::SetTooltip("Every PC together: this PC plus the time your other PCs logged (their play logs sync with your saves).");
            x += it.w;
        }
    }
}

std::string catalog_title(const gamepage::Ctx& c) { return c.cat ? c.cat->title : std::string(); }

/* A column of the current window: the items between column_begin and column_end start at screen x and lay out (wrap, fill, align) as if
 * the region were w wide, so the game's sections need no width parameter. Restores the window's region afterwards. */
struct Column {
    float work_max = 0.f, content_max = 0.f;
};
Column column_begin(float x, float y, float w) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    Column c{win->WorkRect.Max.x, win->ContentRegionRect.Max.x};
    const float r = std::min(win->WorkRect.Max.x, x + std::max(1.f, w));
    win->WorkRect.Max.x = r;
    win->ContentRegionRect.Max.x = r;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginGroup();
    return c;
}
float column_end(const Column& c) {   // returns the column's bottom (screen y)
    ImGui::EndGroup();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    win->WorkRect.Max.x = c.work_max;
    win->ContentRegionRect.Max.x = c.content_max;
    return ImGui::GetItemRectMax().y;
}

}  // namespace

namespace gamepage {

Ctx begin(State& st, const hub::GameEntry& e, const Host& host, bool appearing) {
    const hub::GameState& gs = gstate(e.key);
    const GameInfo* cat = catalog_for(e);
    poll_save_info();
    {   /* the save numbers (Saves tab, Play / Resume): the full-window page scans again every 4 s as it always did (pagecache "periodic");
         * the desk pane once the cursor has settled on the game and after pagecache::invalidate(), never while a game runs */
        static std::string s_loaded;   // the game the last scan was asked for, under which pagecache generation
        static unsigned s_gen = 0;
        if (pagecache::host() == pagecache::Host::Pane && save_info_switch(e)) s_loaded.clear();   // another game: its last numbers (or none), nothing read yet; scanned again once settled
        const pagecache::Need n = pagecache::need(A.info_key == e.key && s_loaded == e.key, s_gen, A.info_at, 4.0);
        if (n != pagecache::Need::Use) {
            qadisk::Scope qs(pagecache::scope_name(n), false);
            if (refresh_save_info_bg(e)) { s_loaded = e.key; s_gen = pagecache::generation(); }
        }
    }
    Ctx c{st, e, host, appearing, gs, cat};
    c.idle = hubui::launch_allowed(A.proc != nullptr, session_active());
    c.native = is_native(e);
    entry_colors(e, &c.c0, &c.c1);
    return c;
}

/* ---------- header ---------- */

void draw_cover(Ctx& c, ImDrawList* dl, ImVec2 a0, ImVec2 a1) {
    ArtRef art = art_for_entry(c.e);
    TileSpec ts;
    ts.title = c.e.title;
    ts.c0 = c.c0;
    ts.c1 = c.c1;
    ui::shadow(dl, a0, a1, sp(1.75f), 1.2f);
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.75f));
    else draw_no_art(dl, ts, a0, a1, sp(1.75f), 1.f);
    dl->AddRect(a0, a1, ui::HAIRLINE, sp(1.75f), 0, 1.f);
}

std::string facts_line(const Ctx& c) {
    const hub::GameEntry& e = c.e;
    const char* dot = "  \xC2\xB7  ";
    std::string facts = hub::console_name(e);
    if (c.native) facts += std::string(dot) + "Native port";
    if (local_players_for(e) >= 2) facts += std::string(dot) + "1-" + std::to_string(local_players_for(e)) + " players";
    if (supports_online(e)) facts += std::string(dot) + "Online";
    if (c.cat && c.cat->year) facts += std::string(dot) + std::to_string(c.cat->year);
    return facts;
}

static void pt_refresh_tz(Ctx& c) {
    if (ImGui::GetTime() - c.st.pt_tz_at > 30.0) { c.st.pt_tz = pt_local_tz(); c.st.pt_tz_at = ImGui::GetTime(); }   // mktime twice: not every frame
}

std::string play_time_line(Ctx& c) {
    pt_refresh_tz(c);
    return hubui::pt::header_line(c.gs.play_seconds, c.gs.last_played, (int64_t)std::time(nullptr), c.st.pt_tz, "  \xC2\xB7  ");
}

bool draw_play_time(Ctx& c, float w) {
    /* Steam-style play time: the stats strip (Play time, Last played, Sessions, ...); nothing for a game never played here (the header's
     * play-time line already says "Not played yet") */
    const int64_t pt_now = (int64_t)std::time(nullptr);
    pt_refresh_tz(c);
    const hubui::pt::GameStats& pst = pt_stats_for(c.st.pt, c.e, c.gs, pt_now);
    if (!pst.played()) return false;
    const std::vector<hubui::pt::Cell> cells = hubui::pt::strip_cells(pst, pt_now, c.st.pt_tz);
    if (cells.empty()) return false;
    draw_playtime_strip(cells, w);
    return true;
}

void draw_description(Ctx& c) {
    /* short description (clamped, More expands); falls back to the built-in blurb when nothing loaded */
    bool drew = gameinfo_ui_section_held(c.e, catalog_title(c), 1, nullptr);
    if (!drew && c.cat && !c.cat->blurb.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextWrapped("%s", c.cat->blurb.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
}

void draw_notices(Ctx& c) {
    const hub::GameEntry& e = c.e;
    /* disk facts, cached per game (pagecache): an archived game's unpacked copy, a Wii U disc's keys. An unpack finishing or a key file
     * added (the window gets the focus back) reads them again. */
    static pagecache::PerGame<bool> s_unpacked;
    static pagecache::PerGame<std::string> s_keys_why;
    const bool* unpacked = e.issue == "extract" ? s_unpacked.get(e.key + "|" + e.path, 2.0, [&] { return !hub::extracted_3ds_path(e).empty(); }) : nullptr;
    if (e.issue == "extract" && !unpacked) {
        // not known yet (the desk cursor is still moving, or a game runs): say nothing rather than something that may be wrong
    } else if (e.issue == "extract" && *unpacked) {
        text_wrapped_disabled("Unpacked and ready: Play starts at once.");
    } else if (e.issue == "extract") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("Archived: %s cannot open %s files. Press Play to unpack it (about %.1f GB) into RetroHub's data folder - your archive stays untouched.",
                           e.system == hub::System::PS2 ? "PCSX2" : e.system == hub::System::PSP ? "PPSSPP" : e.system == hub::System::GC ? "Dolphin" : "Azahar", e.system == hub::System::GC || e.system == hub::System::PS2 || e.system == hub::System::PSP ? "archive" : ".zip", unpacked_size(e) / 1073741824.0);
        ImGui::PopStyleColor();
    } else if (e.issue == "encrypted") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("This 3DS image is encrypted. Azahar needs your own AES keys (aes_keys.txt in its user\\sysdata folder) to run it.");
        ImGui::PopStyleColor();
    } else if (e.issue == "keys") {
        const std::string* why = s_keys_why.get(e.key + "|" + A.st.cemu_exe, 2.0, [&] {
            std::string ud;
            hubemu::Install ki = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, e.path);
            if (!ki.exe.empty()) { const std::string ad_s = winutil::getenv_utf8("APPDATA"); const char* ad = ad_s.empty() ? nullptr : ad_s.c_str(); ud = ki.managed ? ki.dir + "\\portable" : hub::cemu_dirs_for(ki.dir, ad ? ad : "", hubemu::wiiu_probe()).user_data; }
            return hub::wiiu_keys_message(hub::wiiu_keys_check(ud, ki.dir, hubemu::wiiu_probe()), "wud");
        });
        if (why) {   // (nothing until it is known)
            ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
            ImGui::TextWrapped("%s", why->empty() ? "Disc images (.wud/.wux) are decrypted with the disc keys in Cemu's keys.txt (found). A .wua archive needs no keys." : why->c_str());
            ImGui::PopStyleColor();
        }
    }
    if (is_kanto(e)) {
        text_wrapped_disabled("A friend's two-player co-op version of Pokemon FireRed. It is played online: press Play to set it up (once), then host or join a room and pick it. Needs your own Pokemon FireRed (USA) 1.0 in the library.");
        if (button("How to play", ImVec2(sp(14.f), sp(4.f)))) kt_open_howto();
    }
}

/* ---------- tabs ---------- */

static void ensure_tabs(Ctx& c) {
    if (c.tabs_ready) return;
    c.tabs_ready = true;
    State& st = c.st;
    const hub::GameEntry& e = c.e;
    if (st.tab_key != e.key || c.appearing) { st.tab_key = e.key; st.tab = TabAbout; if (const char* s = g_headless ? std::getenv("HUB_SHOT_TAB") : nullptr) st.tab = std::atoi(s); }   // HUB_SHOT_TAB=0..3 for screenshots
    qm_seed_gallery(e);   // HUB_SHOT_GALLERY screenshot aid
    const int nshots = shots_count(e);   // -1: not known yet (the desk cursor is still moving, or a game runs): no Screenshots tab for now
    const bool has_settings = gs_has_game_section(e);
    std::vector<TabInfo>& tl = c.tab_list;
    tl.clear();
    tl.push_back({TabAbout, "About"});
    if (!c.native) tl.push_back({TabSaves, "Saves"});
    if (nshots > 0) tl.push_back({TabShots, "Screenshots (" + std::to_string(nshots) + ")"});
    if (has_settings) tl.push_back({TabSettings, "Settings"});
    c.tab_shown = st.tab;   // what is drawn; `st.tab` keeps the request so a gallery that is seeded a frame later still lands on its tab
    bool ok = false;
    for (auto& t : tl) ok |= t.id == st.tab;
    if (!ok) {
        if (g_headless && std::getenv("HUB_SHOT_TAB")) {   // the screenshot aid asked for a tab this game does not have: clamp to the nearest one that exists
            c.tab_shown = tl.back().id;
            for (auto& t : tl) if (t.id >= st.tab) { c.tab_shown = t.id; break; }
        } else if (nshots < 0 && st.tab == TabShots) c.tab_shown = TabAbout;   // the pane keeps the Screenshots tab asked for until the count is known
        else st.tab = c.tab_shown = TabAbout;
    }
}

const std::vector<TabInfo>& tabs(Ctx& c) {
    ensure_tabs(c);
    return c.tab_list;
}

int shown_tab(Ctx& c) {
    ensure_tabs(c);
    return c.tab_shown;
}

void select_tab(Ctx& c, int id) {
    ensure_tabs(c);
    c.st.tab = c.tab_shown = id;
}

/* About: the play-time stats and the description first, then the game's sections. On a wide body the stats and the controllers move to a
 * side column (game_page_layout.h about_columns); the description and the facts (developer, publisher, ...) lead the main column. */
static void draw_about(Ctx& c) {
    const hub::GameEntry& e = c.e;
    const float x0 = ImGui::GetCursorScreenPos().x, y0 = ImGui::GetCursorScreenPos().y;
    const float avail = ImGui::GetContentRegionAvail().x;
    hubui::gp::AboutCols cols = hubui::gp::about_columns(avail, sp(1.f));
    float side_bottom = y0;
    auto players = [&](bool side) {   // offline local multiplayer: who holds which port before Play (the strip wraps in a narrow column)
        if (local_players_for(e) < 2) return;
        if (!side) ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(1.f)));   // wrapped lines sit close together
        const bool reassign = draw_players_strip(local_players_for(e));
        ImGui::PopStyleVar();
        if (reassign) { c.host.close(); open_port_assign(); }
    };
    if (cols.side) {   // side column first: its height does not depend on the main column
        Column sc = column_begin(x0 + cols.side_x, y0, cols.side_w);
        if (draw_play_time(c, cols.side_w)) ImGui::Dummy(ImVec2(0, sp(1.f)));
        players(true);
        side_bottom = column_end(sc);
        if (side_bottom <= y0 + 1.f) cols.main_w = hubui::gp::readable_w(avail, sp(1.f));   // nothing to show there: one column after all
    }
    Column mc = column_begin(x0, y0, cols.main_w);
    if (!cols.side && draw_play_time(c, cols.main_w)) ImGui::Dummy(ImVec2(0, sp(0.75f)));
    draw_description(c);
    dumpwarn_details_section(e);   // a copy that looks incomplete must be seen without scrolling
    nuprepair_details_section(e);   // a damaged .nup copy RetroHub fills in at launch
    gameinfo_ui_section_held(e, catalog_title(c), 2, nullptr);   // facts + ratings
    achievements_ui_section(e);   // RetroAchievements progress (only when signed in)
    if (e.system == hub::System::WiiU) {
        const WiiuFacts wf = wiiu_info_lines(e);
        for (const std::string& l : wf.lines) ImGui::TextDisabled("%s", l.c_str());
        /* player 1's controller: the launch's own decision (install + title id from the scan, the game's settings as they are now);
         * nothing until the scan has landed, so the line never flips from one claim to the other */
        if (wf.ready) ImGui::TextDisabled("%s", hubemu::cemu_player1_hint(cemu_player1_for(e, wf.inst, wf.tid), e, wf.tid).c_str());
    }
    { std::string fl = fr_playtime_line(e.key); if (!fl.empty()) ImGui::TextDisabled("Friends: %s", fl.c_str()); }
    if (c.native) ImGui::TextDisabled("Smash Classic keeps its own saves.");
    if (!cols.side) players(false);
    dupes_details_section(e);
    hack_details_section(e);   // Versions row (Original + hacks) and Remove hack
    progress_details_section(e);   // Wave 3: completion + notes
    transferpak_details_section(e);
    gba_link_details_section(e);
    palpark_details_section(e);
    verify_details_section(e);
    hookshot_details_section(e);   // Ocarina of Time (Switch 2) only: build the native version
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    /* screenshot aid HUB_SHOT_PACKGAME=<key> (headless only): that game shows as installed from a shared pack, so its Uninstall button and
     * confirm can be captured. Its path is not in the pack folder, so a confirmed Uninstall finds nothing to delete (gamepack_uninstall) */
    static const char* shot_pack = g_headless ? std::getenv("HUB_SHOT_PACKGAME") : nullptr;
    const bool fake_pack = shot_pack && e.key == shot_pack;
    if (gamepack_owns(e) || fake_pack) {   // installed from a shared pack: its folder (full path on hover) + Uninstall; the archive file name inside is not interesting
        const std::string ft = fake_pack ? e.title : hubgp::installed_folder_title(e.path, hubgp::pack_root());
        const std::string full = hubgp::install_dir(sys_short(e), ft);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextUnformatted(ui::ellipsize(ui::F.cap, ImGui::GetFontSize(), "Installed in " + sys_short(e) + "\\" + ft + "  (RetroHub games folder)", ImGui::GetContentRegionAvail().x).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full.c_str());
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (gamepack_uninstall_ui(e, sp(5.f))) c.host.close();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextWrapped("%s", e.path.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    const float main_bottom = column_end(mc);
    ImGui::SetCursorScreenPos(ImVec2(x0, std::max(main_bottom, side_bottom)));
    ImGui::Dummy(ImVec2(0, 0));   // a submitted item, so the cursor move counts for the scroll area
}

static void draw_saves(Ctx& c) {
    const hub::GameEntry& e = c.e;
    std::string sv;
    if (A.info_battery_files > 0) sv = "Save data: " + std::to_string(A.info_battery_files) + " file(s), " + size_text(A.info_battery_bytes);
    if (A.info_state_files > 0 || A.info_auto_state) {
        if (!sv.empty()) sv += "   ";
        sv += std::to_string(A.info_state_files) + " save state(s)" + (A.info_auto_state ? " + resume point" : "");
    }
    if (sv.empty() && save_info_known(e.key)) sv = "No save data yet.";   // (not checked yet: the desk cursor is still moving, or a game runs: no line)
    if (!sv.empty()) ImGui::TextUnformatted(sv.c_str());
    cloud_ui_game_line(e);
    history_ui_details_section(e);
    resume_details_section(e);   // Wave 3 R1
    draw_state_strip(e);
}

void draw_tab(Ctx& c, int id) {
    if (id == TabAbout) { draw_about(c); return; }
    if (id == TabShots) { shots_gallery(c.e); return; }   // a grid that fills the width
    /* Saves and Settings: text and option rows at a readable width on a wide body; Settings uses the whole width when its sections flow
     * into two columns (option_sections.h option_flow) */
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool two = id == TabSettings && hubui::option_flow(avail, sp(1.f)).cols > 1;
    Column col = column_begin(p.x, p.y, two ? avail : hubui::gp::readable_w(avail, sp(1.f)));
    if (id == TabSaves) draw_saves(c);
    else if (id == TabSettings) gs_draw_game_section(c.e);
    const float bottom = column_end(col);
    ImGui::SetCursorScreenPos(ImVec2(p.x, bottom));
    ImGui::Dummy(ImVec2(0, 0));
}

/* ---------- actions ---------- */

bool shot_forced(Ctx& c, const char* name) {
    static const char* act = g_headless ? std::getenv("HUB_SHOT_ACT") : nullptr;   // reachability QA (qa/reach_matrix.ps1): HUB_SHOT_ACT=<name> presses that action once; screenshot mode only (a stray variable must never press Play on a real PC)
    static std::string done;   // the game + action already pressed: once per run, whichever host draws the page (pane, then the full-window page)
    if (!act || !name || std::strcmp(act, name) != 0 || done == c.e.key + name) return false;
    if (!SH.details_key.empty() && c.e.key != SH.details_key) return false;   // HUB_SHOT_DETAILS names the game (the desk pane shows another one first)
    done = c.e.key + name;
    return true;
}

static void trace_action(Ctx& c, const Action& a) {
    for (const std::string& t : a.traces) shot_trace_once("act", c.e.key + " " + t);   // every action reports whether it is offered
}

void trace_actions(Ctx& c, const std::vector<Action>& actions) {
    for (const Action& a : actions) trace_action(c, a);
}

std::vector<Action> primary_actions(Ctx& c) {
    const hub::GameEntry& e = c.e;
    const Host host = c.host;
    const bool idle = c.idle;
    std::vector<Action> v;
    {   /* Play (Resume when a resume point exists) */
        const bool resume = !c.native && A.info_auto_state && e.issue.empty();
        Action a;
        a.id = "play";
        a.label = resume ? "Resume" : "Play";
        a.icon = ui::Icon::Play;
        a.width = sp(17.f);
        a.enabled = idle;
        a.primary = true;
        a.default_focus = true;
        a.act = "play";
        a.traces = {std::string("play ") + (idle ? "enabled" : "disabled")};
        if (!idle) a.tooltip = hubui::launch_busy_reason(A.proc != nullptr);
        a.run = [host, &e] { host.close(); hub::GameEntry cp = e; play_entry_anim(cp); };
        v.push_back(std::move(a));
    }
    {   /* Play Online, or why not */
        Action a;
        a.id = "online";
        a.icon = ui::Icon::Globe;
        if (!supports_online(e) && e.system == hub::System::GC) {
            a.label = "Online soon";
            a.enabled = false;
            a.tooltip = "Only GameCube games with a verified online setup can be played online";
            a.traces = {"online explained:online-soon"};
        } else if (!supports_online(e)) {
            a.label = "Offline only";
            a.enabled = false;
            a.tooltip = e.system == hub::System::PSP ? "PSP games play offline only: PPSSPP has no online rooms in RetroHub" : e.system == hub::System::Switch ? "Switch games play offline only for now (Lichii is very experimental)"
                        : !online_block_reason(e).empty() ? online_block_reason(e) : "Online play isn't available for this game";
            a.traces = {"online explained:offline-only"};
        } else {
            a.label = "Play Online";
            a.enabled = idle;
            a.act = "online";
            a.traces = {std::string("online ") + (idle ? "enabled" : "disabled")};
            if (!idle) a.tooltip = hubui::launch_busy_reason(A.proc != nullptr);
            a.run = [host, &e] {
                host.close();
                A.pending_room_game = room_id(e);
                shot_trace("play-online", "pending_room_game=" + A.pending_room_game);
                if (!g_headless) enter_room(true, hubnet::new_room_code());
            };
        }
        v.push_back(std::move(a));
    }
    {
        Action a;
        a.id = "close";
        a.label = "Close";
        a.icon = ui::Icon::Close;
        a.run = [host] { host.close(); };
        v.push_back(std::move(a));
    }
    return v;
}

std::vector<Action> secondary_actions(Ctx& c) {
    const hub::GameEntry& e = c.e;
    const hub::GameState& gs = c.gs;
    const Host host = c.host;
    std::vector<Action> v;
    std::vector<std::string> pending;   // trace lines of actions this game does not offer: written before the next action that is drawn
    auto add = [&](Action a) {
        if (!pending.empty()) { a.traces.insert(a.traces.begin(), pending.begin(), pending.end()); pending.clear(); }
        v.push_back(std::move(a));
    };
    {
        Action a;
        a.id = "favorite";
        a.label = gs.favorite ? "Unfavorite" : "Favorite";
        a.icon = gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star;
        a.act = "favorite";
        a.traces = {"favorite enabled"};
        a.run = [&e, &gs] {
            const bool was_fav = gs.favorite;
            toggle_favorite(e.key);
            shot_trace("favorite", e.key + " was=" + (was_fav ? "1" : "0") + " now=" + (gstate(e.key).favorite ? "1" : "0"));
        };
        add(std::move(a));
    }
    if (!c.native) {
        Action a;
        a.id = "savefolder";
        a.label = "Save folder";
        a.icon = ui::Icon::Folder;
        a.act = "savefolder";
        a.traces = {"savefolder enabled"};
        a.run = [&e] {
            std::string sd = hub::saves::game_dir(sys_short(e), e.key);
            shot_trace("savefolder", sd);
            if (!g_headless) open_in_explorer(sd);
        };
        add(std::move(a));
    } else {
        Action a;
        a.id = "smash";
        a.label = "Find Smash 64 game";
        a.traces = {"savefolder hidden:native-keeps-own-saves"};
        a.poll = [](bool pressed) { locate_smash(pressed); };
        add(std::move(a));
    }
    const bool cheats_na = !cheats_supported(e) && !is_native(e) && !is_kanto(e);   // GC / 3DS / Wii U / PS2: shown disabled, with the reason
    pending.push_back(std::string("cheats ") + (cheats_supported(e) ? "enabled" : cheats_na ? "explained:unavailable" : "hidden"));
    if (cheats_na) {
        Action a;
        a.id = "cheats";
        a.label = "Cheats";
        a.icon = ui::Icon::Bolt;
        a.enabled = false;
        a.tooltip = std::string("Cheats aren't available for ") + hub::console_name(e) + " yet";
        add(std::move(a));
    }
    if (cheats_supported(e)) {
        static bool cheats_read = false;   // the cheat list is read from the vault once (then kept): not while the desk cursor is moving
        const int cn = cheats_read || pagecache::may_read() ? cheats_enabled_count(e) : 0;
        cheats_read = cheats_read || pagecache::may_read();
        Action a;
        a.id = "cheats";
        a.label = cn ? "Cheats (" + std::to_string(cn) + ")" : std::string("Cheats");
        a.icon = ui::Icon::Bolt;
        a.primary = cn > 0;
        a.act = "cheats";
        a.run = [host, &e] { host.close(); hub::GameEntry cp = e; cheats_open(cp); shot_trace("cheats", "opened " + e.key); };
        add(std::move(a));
    }
    pending.push_back("controls " + (hubctl::console_key(e).empty() ? std::string("hidden") : "enabled:" + hubctl::console_key(e)));
    pending.push_back("settings " + (hub::gs::options_for(e.system).empty() ? std::string("none") : "options=" + std::to_string(hub::gs::options_for(e.system).size())));
    if (!hubctl::console_key(e).empty()) {
        Action a;
        a.id = "controls";
        a.label = "Controls";
        a.icon = ui::Icon::Gamepad;
        a.act = "controls";
        a.run = [host, &e] { host.close(); open_controls_for(e); shot_trace("controls", "tab-for " + hubctl::console_key(e) + " game=" + CTL_game_for_trace()); };
        add(std::move(a));
    }
    {
        Action a;
        a.id = "desktop";
        a.label = "Add to desktop";
        a.icon = ui::Icon::Star;
        a.act = "desktop";
        a.traces = {"desktop enabled"};
        a.run = [&e] { add_game_shortcut(e); shot_trace("desktop", e.key); };
        add(std::move(a));
    }
    {
        Action a;   // rename / cover / reset (customize_ui.cpp)
        a.id = "customize";
        a.label = "Customize";
        a.icon = ui::Icon::Gear;
        a.run = [&e] { custom_open_customize(e.key); };
        add(std::move(a));
    }
    {
        Action a;
        a.id = "shelf";
        a.label = "Add to shelf...";
        a.icon = ui::Icon::Plus;
        a.run = [&e] { custom_open_picker(e.key, true); };
        add(std::move(a));
    }
    if (hack_can_add(e)) {   // ROM hacks and translations (hack_ui.cpp): pick a patch file, RetroHub finds the base game and adds a new entry
        Action a;
        a.id = "hack";
        a.label = "Add hack or translation";
        a.icon = ui::Icon::Plus;
        a.run = [&e] { hack_add_open(e); };   // the page stays: its picker and dialog open on top of it (hack_page_dialogs)
        add(std::move(a));
    }
    if (!c.native) {   // Hide game (library-12): one press, Undo card after; synced like the Hidden games list
        Action a;
        a.id = "hide";
        a.label = "Hide game";
        a.icon = ui::Icon::Close;
        a.ends_page = true;
        const std::string hk = e.key;
        a.run = [hk] { hide_game_now(hk); };   // the entry is gone from the library after this: nothing may use it
        add(std::move(a));
    }
    return v;
}

}  // namespace gamepage
