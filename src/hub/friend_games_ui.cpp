#include "app.h"
#include "gameinfo.h"
#include "gamepack.h"
#include <chrono>

bool gameinfo_ui_section_query(const gameinfo::Query& q, const std::string& id, int part = 0);   // gameinfo_ui.cpp: description, ratings, facts

/* Friends' games shown greyed out in the library (metadata only: titles and ids, never files).
 *   - ghost_tick():   publishes THIS PC's game list to the friends server (debounced, only when it changed, only when
 *                     "Show my games to friends" is on) and re-composes the library when a friend's list changed.
 *   - rebuild_ghosts(): friends' games you do not own, de-duplicated by game identity (friend_games.h).
 *   - tiles / bar / details: greyed cover, friend avatars, "Ask <friend> to host", "Add your own copy".
 * No polling loops: lists are fetched by FriendsClient only when a friend's version number changed. */

/* ---------- state ---------- */

static std::vector<hubui::fg::Owned> owned_snapshot() {
    std::vector<hubui::fg::Owned> v;
    v.reserve(A.lib.size());
    const std::string root = hubgp::pack_root();
    for (const hub::GameEntry& e : A.lib) {
        v.push_back({sys_short(e), e.key, e.catalog_id, e.title});
        std::string t = hubgp::installed_folder_title(e.path, root);   // installed from a shared pack: also owned under the pack's title (a catalog title may differ from it)
        if (!t.empty()) v.push_back({sys_short(e), e.key, e.catalog_id, t});
    }
    return v;
}

const hubui::fg::Ghost* find_ghost(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& g : A.ghosts) if (g.id == id) return &g;
    return nullptr;
}

void rebuild_ghosts() {
    A.ghosts.clear();
    std::vector<std::pair<std::string, const std::vector<hubnet::LibGame>*>> libs;
    if (A.st.show_friend_games && A.friends_chip) fr_library_collect(&libs);   // an empty own library still gets ghosts: new users install from shared packs
    const std::vector<hubui::fg::Owned> mine = owned_snapshot();
    if (!libs.empty()) A.ghosts = hubui::fg::build_ghosts(mine, libs);
    const size_t friend_ghosts = A.ghosts.size();
    if (A.st.show_friend_games && A.friends_chip && A.ghosts.size() < hubui::fg::kMaxGhosts) {   // games in the shared Drive packs, owned by no friend
        std::vector<hubui::fg::Ghost> pk = hubgp::pack_only_ghosts(mine, A.ghosts, hubgp::index_games(), hubui::fg::kMaxGhosts - A.ghosts.size());
        A.ghosts.insert(A.ghosts.end(), pk.begin(), pk.end());
    }
    static std::string last;
    const size_t nf = fr_friends().size();
    std::string line = "ghosts: friends=" + std::to_string(nf) + " lists_held=" + std::to_string(libs.size()) +
                       " built=" + std::to_string(friend_ghosts) + " pack=" + std::to_string(A.ghosts.size() - friend_ghosts) + " own=" + std::to_string(A.lib.size());
    std::string why = hubui::fg::empty_reason(A.st.show_friend_games && A.friends_chip, nf, libs.size(), A.ghosts.size());
    if (!why.empty()) line += " (empty: " + why + ")";
    if (line != last) { last = line; online_log(line); }
}

/* ---------- GamePack: install a greyed-out game from the shared Drive folder (gamepack.cpp) ---------- */

static hubgp::Installer GP;   // one install at a time

static void gamepack_finished(bool success) {
    if (success) {   // the games folder joins the library, then a rescan picks the game up (and drops it from the ghosts)
        if (hubui::merge_installer_games_dir(&A.st, hubgp::pack_root())) mark_dirty();
        start_scan();
        set_toast(GP.title + " installed.");
    } else if (GP.error == "cancelled") {
        GP.state = hubgp::Installer::Idle;   // the user stopped it: no error dialog
    }
}

void poll_gamepack() {
    bool success = false;
    if (GP.poll(&success)) gamepack_finished(success);
}

void gamepack_shutdown() {
    GP.request_cancel();
    if (GP.th.joinable()) GP.th.join();
    if (GP.state == hubgp::Installer::Working) gamepack_finished(GP.ok);   // finished on the last frame: register a successful install
    hubgp::index_shutdown();
}

void draw_gamepack_dialogs() {
    if (GP.state == hubgp::Installer::Idle) return;
    bool open = false;
    emu_modal_begin("Install game", &open);
    if (open) {
        const bool back = ui::modal_back();
        if (GP.state == hubgp::Installer::Working) {
            ImGui::TextUnformatted(("Installing " + GP.title).c_str());
            int fi = GP.file_idx, fc = GP.file_count;
            if (fc > 1) { char t[32]; std::snprintf(t, sizeof t, "file %d of %d", fi, fc); ImGui::TextDisabled("%s", t); }
            std::string cur;
            { std::lock_guard<std::mutex> lk(GP.mu); cur = GP.current; }
            if (!cur.empty()) ImGui::TextWrapped("%s", cur.c_str());
            uint64_t got = GP.got, tot = GP.total;
            char ov[48];
            std::snprintf(ov, sizeof ov, "%.1f / %.1f MB", got / 1048576.0, tot / 1048576.0);
            ImGui::ProgressBar(tot ? (float)got / (float)tot : 0.f, ImVec2(-1, sp(3.f)), ov);
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            if (button("Cancel", ImVec2(sp(20.f), sp(5.5f)))) GP.request_cancel();
        } else {   // Failed (a cancel never gets here: poll_gamepack() closes it silently)
            if (back) GP.state = hubgp::Installer::Idle;
            std::string err;
            { std::lock_guard<std::mutex> lk(GP.mu); err = GP.error; }
            ImGui::TextWrapped("Could not install %s: %s", GP.title.c_str(), err.c_str());
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            if (button("Close", ImVec2(sp(20.f), sp(5.5f)), true)) GP.state = hubgp::Installer::Idle;
            ImGui::SetItemDefaultFocus();
        }
        if (GP.state == hubgp::Installer::Idle) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* Screenshots (HUB_SHOT_FAKE_FRIENDS + HUB_SHOT_FAKE_FRIEND_LIBS=<n extra made-up games for one friend>): fake friends' libraries. */
void fg_fake_libs() {
    const char* env = std::getenv("HUB_SHOT_FAKE_FRIEND_LIBS");
    if (!env) return;
    int extra = std::max(0, std::atoi(env));
    const std::vector<GameInfo>& cat = hub::catalog();
    auto add = [&](const char* uid, size_t step, size_t off) {
        std::vector<hubnet::LibGame>& v = FR.f_libs[uid];
        for (size_t i = off; i < cat.size(); i += step) {
            const GameInfo& g = cat[i];
            if (g.engine == hub::Engine::Native || g.id == "remix") continue;
            v.push_back({g.system, g.system + ":fake-" + g.id, g.id, g.title});
        }
    };
    const char* rg = std::getenv("HUB_SHOT_FAKE_REAL_GHOSTS");
    if (!(rg && *rg == '2')) add("miauid", 2, 0);
    if (!(rg && *rg == '2')) { add("jakuid", 3, 1); add("lunuid", 5, 2); add("devuid", 4, 3); }
    if (std::getenv("HUB_SHOT_FAKE_REAL_GHOSTS")) {   // real titles with real identity keys, no catalog id: exercises DAT art resolution
        std::vector<hubnet::LibGame>& r = FR.f_libs["miauid"];
        r.push_back({"ps1", "ps1:slus-01188", "", "Woody Woodpecker Racing"});
        r.push_back({"ps1", "ps1:scus-94570", "", "Crash Bash"});
        r.push_back({"ps2", "ps2:slus-20398", "", "Road Trip"});
        r.push_back({"snes", "snes:fake", "", "Super Mario World"});
        r.push_back({"nes", "nes:fake", "", "Super Mario Bros."});
    }
    if (rg && *rg == '2') return;
    static const char* sys[] = {"n64", "gc", "gba", "gb", "gbc", "nds", "3ds", "ps1"};
    std::vector<hubnet::LibGame>& big = FR.f_libs["kaiuid"];
    for (int i = 0; i < extra; ++i) {
        std::string t = "Friend Game " + std::to_string(1000 + i);
        big.push_back({sys[i % 8], std::string(sys[i % 8]) + ":kai-" + std::to_string(i), "", t});
    }
}

/* ---------- publish + refresh ---------- */

void ghost_tick() {
    static double next = 0;
    static hubui::fg::PublishGate gate;
    static uint32_t seen_epoch = 0xffffffffu;
    static size_t seen_friends = (size_t)-1;
    static bool seen_chip = true;
    double now = ImGui::GetTime();
    if (now < next) return;
    next = now + 1.0;

    /* shared pack index: which greyed-out games are installable (built-in pack + extra roots from Settings > Online).
     * Re-runs when the links change or 10 min pass (the pack gains games); a failed run retries on that same cadence. */
    static std::string seen_pack_links;
    static unsigned seen_ix = 0xffffffffu;
    {
        const std::string& links = A.st.pack_links;
        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();   // same clock as index_last_attempt_ms()
        long long last_ms = hubgp::index_last_attempt_ms();
        const bool recheck = hubgp::take_recheck();   // Rescan: check the Drive packs again right away
        if ((recheck || links != seen_pack_links || last_ms == 0 || now_ms - last_ms > 600000)) {
            seen_pack_links = links;
            hubgp::index_refresh(hubgp::parse_links(links));
        }
    }

    if (unsigned ix = hubgp::index_epoch(); ix != seen_ix) { seen_ix = ix; A.vis_dirty = true; }   // the pack index found games: rebuild the pack ghosts

    /* a friend's list changed (or the friends list / chip did): recompose the library */
    uint32_t ep = fr_library_epoch();
    size_t nf = fr_friends().size();
    if (ep != seen_epoch || nf != seen_friends || seen_chip != (A.st.show_friend_games && A.friends_chip)) {
        seen_epoch = ep; seen_friends = nf; seen_chip = A.st.show_friend_games && A.friends_chip;
        A.vis_dirty = true;
    }

    if (g_headless && !A.ghosts.empty() && std::getenv("HUB_SHOT_GHOST_OPEN")) {   // screenshots: open the first friend game's details (HUB_SHOT_GHOST_OPEN=<index>)
        static bool done = false;
        if (!done) {
            done = true;
            size_t ix = std::min<size_t>((size_t)std::max(0, std::atoi(std::getenv("HUB_SHOT_GHOST_OPEN"))), A.ghosts.size() - 1);
            A.sel_ghost = A.ghosts[ix].id; A.sel_game.clear(); A.show_ghost = true;
        }
    }

    if (g_headless && std::getenv("HUB_SHOT_GHOST_PACK") && now > 14.0) {   // screenshots: open the first pack-only ghost whose title contains HUB_SHOT_GHOST_PACK (after the index had time to load)
        static bool done = false;
        if (!done) {
            done = true;
            const std::string want = std::getenv("HUB_SHOT_GHOST_PACK");
            for (const auto& g : A.ghosts)
                if (g.uids.empty() && g.title.find(want) != std::string::npos) { A.sel_ghost = g.id; A.sel_game.clear(); A.show_ghost = true; break; }
        }
    }

    /* my list -> the server. Signature = share flag + every game key; unchanged = nothing is ever sent. */
    if (!A.scanned_once || A.scanning) return;
    uint64_t h = 1469598103934665603ull;
    for (const hub::GameEntry& e : A.lib) {
        for (unsigned char c : e.key) { h ^= c; h *= 1099511628211ull; }
        for (unsigned char c : e.title) { h ^= c; h *= 1099511628211ull; }
        h ^= 0x1f; h *= 1099511628211ull;
    }
    std::string sig = std::string(A.st.share_games ? "1:" : "0:") + std::to_string(A.lib.size()) + ":" + std::to_string(h);
    const bool withdraw = !A.st.share_games;
    if (!gate.poll(sig, (long long)(now * 1000.0), withdraw ? 0 : 3000)) return;
    fr_publish_library(A.st.share_games, A.st.share_games ? hubui::fg::publish_list(owned_snapshot(), kNativeKey) : std::vector<hubnet::LibGame>());
}

/* ---------- tiles ---------- */

static std::vector<std::string> ghost_names(const hubui::fg::Ghost& g) {
    std::vector<std::string> n;
    for (const std::string& u : g.uids) if (const hubnet::Friend* f = fr_find(u)) n.push_back(f->name);
    return n;
}

ArtRef art_for_ghost(const hubui::fg::Ghost& g) {
    int w = 0, h = 0;
    SDL_Texture* t = hubart::get_ghost(g.sys, g.key, g.cid, g.title, hubart::Kind::Boxart, &w, &h);
    return art_lookup("ghost:" + g.id, true, t, w, h);
}

TileSpec spec_for_ghost(const hubui::fg::Ghost& g, const char* shelf, bool show_sys) {
    TileSpec t;
    t.id = std::string(shelf) + "|ghost|" + g.id;
    t.title = g.title;
    hub::System sy = hub::system_from_short(g.sys);
    if (show_sys) t.badge = sys_badge(sy);
    system_colors(sy, &t.c0, &t.c1);
    t.c0 = mix(t.c0, IM_COL32(92, 95, 104, 255), 0.72f);   // no-cover tiles are grey too
    t.c1 = mix(t.c1, IM_COL32(70, 73, 82, 255), 0.72f);
    t.owned = false;       // greyed, no star
    t.show_star = false;
    t.selected = A.sel_ghost == g.id;
    t.ghost = &g;
    return t;
}

void handle_ghost_result(const hubui::fg::Ghost& g, const TileResult& r) {
    if (r.focused && A.sel_ghost != g.id) { A.sel_ghost = g.id; A.sel_game.clear(); }
    if (r.clicked || r.activated || r.details) {
        A.sel_ghost = g.id;
        A.sel_game.clear();
        A.show_ghost = true;   // draw_details_modal() opens the friend-game view
    }
}

void ghost_tile_decor(ImDrawList* dl, const hubui::fg::Ghost& g, ImVec2 p0, ImVec2 p1, float e, std::string* subtitle) {
    /* desaturate: a neutral grey wash over the cover (lighter on hover so it is clear it can be opened) */
    dl->AddRectFilled(p0, p1, IM_COL32(96, 99, 108, (int)(150.f - 70.f * e)), sp(1.75f));
    std::vector<std::string> names = ghost_names(g);
    *subtitle = hubui::fg::badge_text(names);
    float r = sp(1.45f);
    size_t shown = std::min<size_t>(3, g.uids.size());
    for (size_t i = 0; i < shown; ++i) {
        const hubnet::Friend* f = fr_find(g.uids[i]);
        if (!f) continue;
        ImVec2 c(p1.x - sp(2.25f) - (float)i * r * 1.35f, p0.y + sp(2.25f));
        fr_avatar(dl, c, r, f->name, fr_row(*f).state, IM_COL32(6, 7, 12, 255));
    }
    if (g.uids.size() > shown) {
        char more[16];
        std::snprintf(more, sizeof more, "+%d", (int)(g.uids.size() - shown));
        ImVec2 ts = ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, more);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p1.x - sp(2.25f) - (float)shown * r * 1.35f - ts.x - sp(0.25f), p0.y + sp(2.25f) - ts.y * 0.5f),
                    IM_COL32(255, 255, 255, 230), more);
    }
}

/* ---------- selected bar (All Games) ---------- */

void selected_bar_ghost(const hubui::fg::Ghost& g) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x, h = sp(8.f);
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ui::PANEL, sp(2.f));
    dl->AddRect(p0, p1, ui::HAIRLINE, sp(2.f), 0, 1.f);
    TileSpec ts = spec_for_ghost(g, "bar", false);
    float th = h - sp(2.f), tw = th * kTileAspect;
    ImVec2 a0(p0.x + sp(1.f), p0.y + sp(1.f)), a1(a0.x + tw, a0.y + th);
    ArtRef art = art_for_ghost(g);
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.25f), 0.8f);
    else draw_no_art(dl, ts, a0, a1, sp(1.25f), 0.8f);
    dl->AddRectFilled(a0, a1, IM_COL32(118, 122, 132, 120), sp(1.25f));
    float bh = sp(5.f), bw = sp(16.f);
    float tx = a1.x + sp(2.f);
    float text_w = std::max(sp(8.f), p1.x - sp(2.f) - bw - tx - sp(2.f));
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(tx, p0.y + sp(1.25f)), ui::TEXT, ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, g.title, text_w).c_str());
    std::string sub = std::string(hub::system_name(hub::system_from_short(g.sys))) + "  -  " + hubui::fg::badge_text(ghost_names(g));
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + sp(1.25f) + ui::F.h2->FontSize + sp(0.5f)), ui::TEXT2,
                ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, sub, text_w).c_str());
    ImGui::SetCursorScreenPos(ImVec2(p1.x - sp(2.f) - bw, p0.y + (h - bh) * 0.5f));
    if (button("Details", ImVec2(bw, bh), true, true, ui::Icon::Dots)) A.show_ghost = true;
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, 1));
}

/* ---------- details view ---------- */

void draw_ghost_modal() {
    if (A.show_ghost) {
        A.show_ghost = false;
        if (find_ghost(A.sel_ghost)) ImGui::OpenPopup("##ghost");
    }
    const hubui::fg::Ghost* gp = find_ghost(A.sel_ghost);
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(sp(84.f), io.DisplaySize.x * 0.92f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##ghost", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (!gp || ui::modal_back()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const hubui::fg::Ghost& g = *gp;
    const GameInfo* cat = g.cid.empty() ? nullptr : hub::find_game(g.cid);
    hub::System sy = hub::system_from_short(g.sys);
    ImU32 c0, c1;
    system_colors(sy, &c0, &c1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    bool with_art = avail > sp(56.f);
    float aw = with_art ? sp(24.f) : 0, ah = aw / kTileAspect;
    if (with_art) {
        ArtRef art = art_for_ghost(g);
        TileSpec ts;
        ts.title = g.title; ts.c0 = c0; ts.c1 = c1;
        ImVec2 a0 = o, a1(o.x + aw, o.y + ah);
        ui::shadow(dl, a0, a1, sp(1.75f), 1.2f);
        if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.75f), 0.85f);
        else draw_no_art(dl, ts, a0, a1, sp(1.75f), 0.85f);
        dl->AddRectFilled(a0, a1, IM_COL32(118, 122, 132, 110), sp(1.75f));
        dl->AddRect(a0, a1, ui::HAIRLINE, sp(1.75f), 0, 1.f);
    }
    float tx = with_art ? aw + sp(3.f) : 0;
    ImGui::SetCursorScreenPos(ImVec2(o.x + tx, o.y));
    ImGui::BeginGroup();
    float tw = avail - tx;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
    ImGui::PushFont(ui::F.title);
    ImGui::TextWrapped("%s", g.title.c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    pill(hub::system_name(sy), mix(c0, IM_COL32_BLACK, 0.2f));
    if (cat && cat->year) { ImGui::SameLine(0, sp(0.75f)); pill(std::to_string(cat->year).c_str(), ui::PANEL2); }
    int players = cat ? cat->max_players : 0;
    if (players > 0) {
        char pl[32];
        std::snprintf(pl, sizeof pl, "1-%d players", players);
        ImGui::SameLine(0, sp(0.75f));
        pill(pl, ui::PANEL2);
    }
    if (cat && !cat->blurb.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextWrapped("%s", cat->blurb.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
    ImGui::TextUnformatted("You don't have this game.");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    if (with_art) ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + std::max(ah, ImGui::GetItemRectSize().y) + sp(2.f)));
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    {   // the same info block as owned games: description, ratings, facts (looked up by system + title)
        gameinfo::Query q;
        q.system = g.sys;
        q.title = g.title;
        q.alt_title = cat ? cat->title : std::string();
        gameinfo_ui_section_query(q, "ghost:" + g.id);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }

    /* who has it */
    const bool pack_only = g.uids.empty();
    if (!pack_only) section_title("Friends who have it");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const size_t kRows = pack_only ? 0 : 6;
    float rh = sp(5.f), bw = sp(22.f);
    for (size_t i = 0; i < g.uids.size() && i < kRows; ++i) {
        const hubnet::Friend* f = fr_find(g.uids[i]);
        if (!f) continue;
        hubui::FriendRow fr = fr_row(*f);
        bool online = fr.state != hubui::FS_Offline;
        ImGui::PushID((int)i);
        ImVec2 r0 = ImGui::GetCursorScreenPos();
        float rw = ImGui::GetContentRegionAvail().x;
        fr_avatar(dl, ImVec2(r0.x + sp(2.f), r0.y + rh * 0.5f), sp(2.f), f->name, fr.state, ui::PANEL);
        std::string state = !online ? "Offline" : f->status.state == hubnet::PresenceState::InGame ? "Playing " + f->status.game_title
                          : f->status.state == hubnet::PresenceState::InLobby ? "In a room" : "Online";
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(r0.x + sp(5.f), r0.y + sp(0.5f)), ui::TEXT, f->name.c_str());
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(r0.x + sp(5.f), r0.y + sp(0.5f) + ui::F.bold->FontSize * 1.25f), ui::TEXT2,
                    ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, state, rw - sp(5.f) - bw - sp(2.f)).c_str());
        ImGui::SetCursorScreenPos(ImVec2(r0.x + rw - bw, r0.y + (rh - sp(4.f)) * 0.5f));
        std::string lab = "Ask " + f->name + " to host";
        if (ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, lab.c_str()).x > bw - sp(3.f)) lab = "Ask to host";
        if (button(lab.c_str(), ImVec2(bw, sp(4.f)), false, online)) fr_ask_host(f->uid, g.cid.empty() ? g.key : g.cid, g.title, g.sys);
        if (!online && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s is offline", f->name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rh));
        ImGui::Dummy(ImVec2(rw, 0));
        ImGui::PopID();
    }
    if (g.uids.size() > kRows) text_wrapped_disabled("and " + std::to_string(g.uids.size() - kRows) + " more");
    if (pack_only) text_wrapped_disabled("In a shared pack. Install it to play.");
    else text_wrapped_disabled("They get a request to open a room with this game. Only the game's name is shared, never game files.");
    ImGui::Dummy(ImVec2(0, sp(1.5f)));

    /* actions: Play / Play Online are shown but disabled (you cannot play what you do not have) */
    float bh = sp(5.5f);
    button("Play", ImVec2(sp(12.f), bh), false, false, ui::Icon::Play);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("You don't have this game");
    ImGui::SameLine(0, sp(1.5f));
    button("Play Online", ImVec2(sp(16.f), bh), false, false, ui::Icon::Globe);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("You don't have this game");
    bool installable = false;
    {   // install from the shared packs (built-in folder + community list + extra roots from Settings > Online)
        const bool builtin = hubgp::sys_folder(g.sys)[0] != '\0';
        const hubgp::IndexState ist = hubgp::index_state(g.sys);
        if (builtin || (ist == hubgp::IndexState::Ready && hubgp::index_source(g.sys))) {
            ImGui::SameLine(0, sp(1.5f));
            bool have = hubgp::installed(g.sys, g.title);
            bool in_pack = ist == hubgp::IndexState::Ready ? hubgp::index_has(g.sys, g.title) : true;   // unknown yet: checked live on click
            if (g_headless && std::getenv("HUB_SHOT_GHOST_INSTALLABLE")) in_pack = true;   // screenshots: pretend the shared pack has this game
            installable = !have && in_pack && GP.state == hubgp::Installer::Idle;
            if (button("Install", ImVec2(sp(12.f), bh), true, installable, ui::Icon::Cloud)) {
                ImGui::CloseCurrentPopup();
                GP.start(g.sys, g.title, hubgp::parse_links(A.st.pack_links));
            }
            if (installable) ImGui::SetItemDefaultFocus();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (have) ImGui::SetTooltip("Already installed");
                else if (!in_pack) ImGui::SetTooltip("Not in any shared pack yet. The owner can add it to their Google Drive and send the link so everyone can install it.");
            }
        }
    }
    ImGui::SameLine(0, sp(1.5f));
    if (button("Add your own copy", ImVec2(sp(21.f), bh), !installable, true, ui::Icon::Plus)) { ImGui::CloseCurrentPopup(); add_games_open(); }
    ImGui::SameLine(0, sp(1.5f));
    if (button("Close", ImVec2(sp(9.f), bh))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
