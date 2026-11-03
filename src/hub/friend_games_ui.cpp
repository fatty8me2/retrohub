#include "app.h"
#include "gameinfo.h"
#include "gamepack.h"
#include "art.h"
#include <chrono>

#include <map>
#include <thread>

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
        if (is_hack(e)) continue;   // ROM hacks / translations are the player's own: never in the published friends' library (Plan 4 H2)
        v.push_back({sys_short(e), e.key, e.catalog_id, e.title});
        {   /* also owned under its clean No-Intro name: an owned copy titled by its ROM header must not leave the pack's ghost of the same game */
            std::string nm = hubui::fg::clean_dat_name(hubart::cached_name(e.key));
            if (!nm.empty()) v.push_back({sys_short(e), e.key, e.catalog_id, nm});
        }
        std::string t = hubgp::installed_folder_title(e.path, root);   // installed from a shared pack: also owned under the pack's title (a catalog title may differ from it)
        if (!t.empty()) v.push_back({sys_short(e), e.key, e.catalog_id, t});
        for (const auto& o : hubgp::pack_owned_aliases(e, root)) v.push_back(o);   // ... and under the pack console folder (a PS2 disc filed under GameCube still clears its GameCube ghost)
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
    if (!libs.empty()) A.ghosts = hubui::fg::build_ghosts(mine, libs, [](const std::string&, const std::string& key) { return hubart::cached_name(key); });
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

/* ---------- GamePack: install a greyed-out game from the shared Drive folder (gamepack.cpp) ----------
 * Installs run in the background, one at a time, with a queue. The UI stays usable: the ghost tile shows its own progress bar,
 * a small pill at the bottom shows the overall state (with Cancel), and a toast with a Play action announces the result.
 * When a game lands it is scanned on its own and swapped in place of the greyed tile (no full-library rescan). */

static struct SizeProbe { std::string key; std::atomic<int> state{0}; std::atomic<uint64_t> bytes{0}; std::thread th; } SP;   // state 0 idle, 1 running, 2 done

static hub::GameEntry g_pending_uninstall;
static bool g_has_pending_uninstall = false;

static hubgp::Installer GP;   // the running job
struct GpItem { std::string sys, title, ghost_id; };
static GpItem GA;                          // what GP is installing
static std::vector<GpItem> GQ;             // waiting, in order
static std::map<std::string, std::string> GFAIL;   // ghost id -> plain-English reason of the last failure (cleared by Retry)
static std::vector<hub::GameEntry> g_pack_added;   // entries installed this session: apply_scan() keeps them even if an older scan lands
bool g_ghost_popup_open = false;           // the friend-game details popup was drawn this frame

static std::string lower_path(std::string s) { for (char& c : s) { if (c == '/') c = '\\'; c = (char)std::tolower((unsigned char)c); } return s; }

static bool path_under(const std::string& path, const std::string& dir) {
    auto norm = [](std::string s) { for (char& c : s) { if (c == '/') c = '\\'; c = (char)std::tolower((unsigned char)c); } return s; };
    std::string p = norm(path), d = norm(dir);
    if (!d.empty() && d.back() != '\\') d += '\\';
    return p.compare(0, d.size(), d) == 0;
}

bool gamepack_busy() { return GP.state == hubgp::Installer::Working || !GQ.empty(); }

void gamepack_enqueue(const std::string& sys, const std::string& title, const std::string& ghost_id) {
    if (sys.empty() || title.empty()) return;
    GFAIL.erase(ghost_id);
    if (GP.state == hubgp::Installer::Working && GA.ghost_id == ghost_id) return;
    for (const GpItem& q : GQ) if (q.ghost_id == ghost_id) return;
    GQ.push_back({sys, title, ghost_id});
    if (gamepack_busy() && GP.state == hubgp::Installer::Working) set_toast(title + " is queued.", true, 4.0);
}

void gamepack_cancel(const std::string& ghost_id) {
    if (GP.state == hubgp::Installer::Working && GA.ghost_id == ghost_id) { GP.request_cancel(); return; }
    GQ.erase(std::remove_if(GQ.begin(), GQ.end(), [&](const GpItem& q) { return q.ghost_id == ghost_id; }), GQ.end());
}

/* 0 = not installing, 1 = waiting in the queue, 2 = running. *frac = 0..1 (-1 unknown), *label = short text for the tile. */
int gamepack_state_for(const std::string& ghost_id, float* frac, std::string* label) {
    if (GP.state == hubgp::Installer::Working && GA.ghost_id == ghost_id) {
        int ph = GP.phase;
        float f = GP.fraction();
        if (frac) *frac = ph == hubgp::Installer::Preparing ? (GP.prep_total ? (float)((double)GP.prep_got / (double)GP.prep_total) : -1.f) : f;
        if (label) {
            char t[48];
            if (ph == hubgp::Installer::Looking) *label = "Starting...";
            else if (ph == hubgp::Installer::Downloading) { if (f >= 0) { std::snprintf(t, sizeof t, "Installing %d%%", (int)(f * 100)); *label = t; } else *label = "Installing..."; }
            else if (ph == hubgp::Installer::Finishing) *label = "Finishing...";
            else *label = "Preparing...";
        }
        return 2;
    }
    for (const GpItem& q : GQ) if (q.ghost_id == ghost_id) { if (frac) *frac = -1.f; if (label) *label = "Queued"; return 1; }
    return 0;
}

/* The one-line status of the running install: "Downloading 1.2 GB of 2.8 GB - 5.2 MB/s - about 3 min left". */
static std::string gamepack_status_line() {
    int ph = GP.phase;
    if (ph == hubgp::Installer::Looking) return "Looking up the game and its size...";
    if (ph == hubgp::Installer::Finishing) return "Adding it to your library...";
    if (ph == hubgp::Installer::Preparing) {
        char t[96];
        std::snprintf(t, sizeof t, "Preparing %s for its first Play (one time): %s of %s", GA.title.c_str(), hubgp::fmt_size(GP.prep_got).c_str(), hubgp::fmt_size(GP.prep_total).c_str());
        return t;
    }
    uint64_t all = GP.all_total, so = GP.bytes_so_far();
    std::string s = all ? hubgp::fmt_size(so) + " of " + hubgp::fmt_size(all) : hubgp::fmt_size(so) + " downloaded";
    double rate = GP.speed.rate();
    std::string r = hubgp::fmt_rate(rate);
    if (!r.empty()) s += "  -  " + r;
    if (all && rate > 0 && all > so) { std::string e = hubgp::fmt_eta((double)(all - so) / rate); if (!e.empty()) s += "  -  " + e; }
    if (GP.resumed) s += "  (continuing an earlier download)";
    return s;
}

static void gamepack_start_next() {
    if (GP.state == hubgp::Installer::Working || GP.th.joinable() || GQ.empty()) return;
    GA = GQ.front();
    GQ.erase(GQ.begin());
    GP.start(GA.sys, GA.title, hubgp::parse_links(A.st.pack_links), true);
}

bool gamepack_owns(const hub::GameEntry& e) { return !hubgp::installed_folder_title(e.path, hubgp::pack_root()).empty(); }

void gamepack_merge_added(std::vector<hub::GameEntry>* lib) {
    for (auto it = g_pack_added.begin(); it != g_pack_added.end();) {
        if (!file_exists(it->path)) { it = g_pack_added.erase(it); continue; }
        bool have = false;
        for (hub::GameEntry& e : *lib) if (lower_path(e.path) == lower_path(it->path)) {
            have = true;
            if (e.catalog_id.empty() && !it->title.empty()) e.title = it->title;   // a zipped cartridge keeps the pack's title (its path is in rom-cache, so no folder name can say it)
            break;
        }
        if (!have) lib->push_back(*it);
        ++it;
    }
}

/* Games found by scanning a pack folder on its own go into the library without a full rescan. Returns the first game's key. */
static std::string gamepack_adopt(std::vector<hub::GameEntry> got, const std::string& pack_title = std::string()) {
    if (hubui::merge_installer_games_dir(&A.st, hubgp::pack_root())) mark_dirty();
    std::string newkey;
    for (hub::GameEntry& e : got) {
        if (e.catalog_id.empty()) {
            std::string t = hubgp::installed_folder_title(e.path, hubgp::pack_root());
            /* a zipped cartridge (.zip N64 / GBA / DS) is listed from rom-cache\, outside the pack folder: no folder name to take the title from,
             * so the greyed tile it replaces would stay (and stay selected, with Play disabled). It takes the title of the install instead. */
            if (t.empty() && got.size() == 1) t = pack_title;
            if (!t.empty() && hubgp::pack_key(t) != hubgp::pack_key(e.title)) e.title = t;
        }
        if (newkey.empty()) newkey = e.key;
        g_pack_added.erase(std::remove_if(g_pack_added.begin(), g_pack_added.end(), [&](const hub::GameEntry& o) { return lower_path(o.path) == lower_path(e.path); }), g_pack_added.end());
        g_pack_added.push_back(e);
    }
    if (got.empty()) return newkey;
    if (A.scanning) {
        /* A scan is running right now and owns A.scan_result (never read it from here). Queue another, and put the new games into the
         * live library right away: before this they only showed up when that scan landed, so Play right after an install found no
         * entry (find_entry() was null) and did nothing until the scan ended - or, if the scan never ran again, until a restart. */
        start_scan();
        bool added = false;
        for (const hub::GameEntry& e : got) {
            bool have = false;
            for (const hub::GameEntry& o : A.lib) if (lower_path(o.path) == lower_path(e.path)) { have = true; break; }
            if (!have) { A.lib.push_back(e); added = true; }
        }
        if (added) {
            A.lib_index.clear();
            for (size_t i = 0; i < A.lib.size(); ++i) A.lib_index.emplace(A.lib[i].key, (int)i);
            for (const hub::GameEntry& e : got) A.lib_state.erase(e.key);   // re-read lazily (a stale state row from a former install)
        }
    } else apply_scan();
    A.vis_dirty = true;
    return newkey;
}

/* A game landed: scan only its folder (done by the install thread), put it in the library, replace the greyed tile in place. */
static void gamepack_landed() {
    std::vector<hub::GameEntry> got;
    std::string note;
    { std::lock_guard<std::mutex> lk(GP.mu); got = GP.entries; note = GP.note; }
    const std::string newkey = gamepack_adopt(got, GA.title);
    if (!newkey.empty() && A.sel_ghost == GA.ghost_id) {   // the greyed tile was the selection: the real game takes its place (same title, same spot)
        A.sel_ghost.clear();
        A.sel_game = newkey;
        if (g_ghost_popup_open) A.show_details = true;   // its friend-game popup turns into the owned game's details
    }
    const std::string title = GA.title;
    if (!newkey.empty()) {
        const hub::GameEntry first = got.front();   // fallback if the library list has not caught up (a scan landing, a duplicate collapse)
        push_toast_action(title + " installed. Ready to play.", COL_GOOD, "Play", [newkey, first]() {
            if (const hub::GameEntry* g = find_entry(newkey)) { A.sel_game = newkey; hub::GameEntry copy = *g; play_entry(copy); }
            else { hub::GameEntry copy = first; play_entry(copy); }
        }, 14.0);
    } else {
        set_toast(title + " installed, but RetroHub did not recognise a game in it. Use Rescan.", false, 10.0);
    }
    if (!note.empty()) set_toast(note, true, 12.0);
}

/* Self-heal: a pack folder with its finished marker but no library entry (an install an older version could not list, or a library cache that lost it)
 * is scanned once per session on its own. The Reinstall button in the game's details is the manual way out. */
static struct HealJob { std::thread th; std::mutex mu; std::vector<hub::GameEntry> found; std::atomic<bool> done{false}; bool started = false, collected = false; } HEAL;

static void gamepack_self_heal_tick() {
    if (!A.scanned_once || A.scanning || HEAL.collected) return;
    if (!HEAL.started) {
        HEAL.started = true;
        std::vector<std::string> dirs;
        std::error_code ec;
        const std::filesystem::path root = std::filesystem::u8path(hubgp::pack_root());
        for (std::filesystem::directory_iterator c(root, ec), ce; !ec && c != ce; c.increment(ec)) {
            if (!c->is_directory(ec)) continue;
            std::error_code e2;
            for (std::filesystem::directory_iterator g(c->path(), e2), ge; !e2 && g != ge; g.increment(e2)) {
                if (!g->is_directory(e2) || !std::filesystem::is_regular_file(g->path() / "gamepack.done", e2)) continue;
                std::string dir = g->path().string();
                bool have = false;
                for (const hub::GameEntry& e : A.lib) if (path_under(e.path, dir)) { have = true; break; }
                if (!have) dirs.push_back(dir);
            }
        }
        if (dirs.empty()) { HEAL.collected = true; return; }
        HEAL.th = std::thread([dirs] {
            std::vector<hub::GameEntry> all;
            for (const std::string& d : dirs) { auto f = hub::scan_library({d}, nullptr); all.insert(all.end(), f.begin(), f.end()); }
            { std::lock_guard<std::mutex> lk(HEAL.mu); HEAL.found = std::move(all); }
            HEAL.done = true;
        });
        return;
    }
    if (!HEAL.done) return;
    if (HEAL.th.joinable()) HEAL.th.join();
    HEAL.collected = true;
    std::vector<hub::GameEntry> got;
    { std::lock_guard<std::mutex> lk(HEAL.mu); got = std::move(HEAL.found); }
    if (!got.empty()) { gamepack_adopt(got); online_log("gamepack: self-heal added " + std::to_string(got.size()) + " installed game(s) the library had missed"); }
}

static void gamepack_finished(bool success) {
    if (success) {
        gamepack_landed();
    } else if (GP.error == "cancelled") {
        // the user stopped it: nothing to show
    } else {
        std::string raw;
        { std::lock_guard<std::mutex> lk(GP.mu); raw = GP.error; }
        std::string msg = hubgp::friendly_error(raw);
        GFAIL[GA.ghost_id] = msg;
        const GpItem failed = GA;
        push_toast_action("Could not install " + failed.title + ". " + msg, COL_BAD, "Retry", [failed]() { gamepack_enqueue(failed.sys, failed.title, failed.ghost_id); }, 14.0);
        online_log("gamepack: install failed: " + failed.title + " (" + raw + ")");
    }
    GP.state = hubgp::Installer::Idle;
}

void poll_gamepack() {
    gamepack_self_heal_tick();
    if (g_has_pending_uninstall) { g_has_pending_uninstall = false; hub::GameEntry e = g_pending_uninstall; gamepack_uninstall(e); }
    if (GP.state == hubgp::Installer::Working) GP.speed.add(ImGui::GetTime(), GP.bytes_so_far());
    bool success = false;
    if (GP.poll(&success)) gamepack_finished(success);
    gamepack_start_next();
}

void gamepack_shutdown() {
    GQ.clear();
    GP.keep = true;   // closing the app mid-download keeps the partial file: the next try continues
    GP.request_cancel();
    if (GP.th.joinable()) GP.th.join();
    if (GP.state == hubgp::Installer::Working && GP.ok) { GP.state = hubgp::Installer::Idle; }
    if (SP.th.joinable()) SP.th.join();
    if (HEAL.th.joinable()) HEAL.th.join();
    hubgp::index_shutdown();
}

/* The uninstall deletes the whole install folder and the unpacked copy, so it is blocked while the running game's ROM / image sits in either
 * (the game itself, or a hack patched on top of a pack game), not just on an exact key match. An unknown running game blocks too. */
static bool pack_uninstall_blocked(const hub::GameEntry& e) {
    if (!A.proc) return false;
    if (A.run.key == e.key) return true;
    const hub::GameEntry* r = find_entry(A.run.key);
    if (!r) return true;
    if (r->hack_base == e.key) return true;
    std::string t = hubgp::installed_folder_title(e.path, hubgp::pack_root());
    if (t.empty()) return false;
    std::string sys = hubgp::installed_folder_console(e.path, hubgp::pack_root());
    if (sys.empty()) sys = sys_short(e);
    return path_under(r->path, hubgp::install_dir(sys, t)) || path_under(r->path, hub::extract_3ds_dir(e));
}

/* Remove an installed game: files, the unpacked copy, the library entry. The greyed entry comes back. Saves stay in the vault. */
void gamepack_uninstall(const hub::GameEntry& e) {
    if (pack_uninstall_blocked(e)) { set_notice("A game that uses these files is running. Close it first."); return; }
    std::string t = hubgp::installed_folder_title(e.path, hubgp::pack_root());
    if (t.empty()) return;
    std::string sys = hubgp::installed_folder_console(e.path, hubgp::pack_root());   // the folder it sits in, which can differ from its console
    if (sys.empty()) sys = sys_short(e);
    const std::string dir = hubgp::install_dir(sys, t);
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(hub::extract_3ds_dir(e)), ec);   // the unpacked copy of an archived disc
    hubgp::uninstall(sys, t);
    g_pack_added.erase(std::remove_if(g_pack_added.begin(), g_pack_added.end(), [&](const hub::GameEntry& o) { return path_under(o.path, dir); }), g_pack_added.end());
    if (A.scanning) start_scan();
    else {
        A.scan_result.erase(std::remove_if(A.scan_result.begin(), A.scan_result.end(), [&](const hub::GameEntry& o) { return path_under(o.path, dir); }), A.scan_result.end());
        apply_scan();
    }
    if (A.sel_game == e.key) A.sel_game.clear();
    A.vis_dirty = true;
    set_toast(e.title + " uninstalled. It is back in the shared packs; your saves are kept.", true, 8.0);
}

/* "Uninstall" button + its confirm popup (details of a pack-installed game). Returns true when the game was removed. */
bool gamepack_uninstall_ui(const hub::GameEntry& e, float bh) {
    bool removed = false;
    const bool running_this = pack_uninstall_blocked(e);   // the running game (or a hack on it) has these files open; other games can be uninstalled
    if (button("Uninstall", ImVec2(0, bh), false, !running_this, ui::Icon::Close)) ImGui::OpenPopup("##gp_uninstall");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(running_this ? "This game is running. Close it to uninstall it." : "Delete this game's files from RetroHub's games folder. It goes back to the shared packs; saves are kept.");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(66.f), ImGui::GetIO().DisplaySize.x * 0.92f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    bool open = ImGui::BeginPopupModal("##gp_uninstall", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    if (open) {
        if (ui::modal_back()) ImGui::CloseCurrentPopup();
        ImGui::TextWrapped("Uninstall %s?", e.title.c_str());
        text_wrapped_disabled("This deletes the game's files from RetroHub's games folder. Your saves stay in the vault. You can install it again from the shared packs at any time.");
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button("Uninstall", ImVec2(sp(18.f), sp(5.5f)), true)) { removed = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(0, sp(1.5f));
        if (button("Keep it", ImVec2(sp(14.f), sp(5.5f)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (removed) { g_pending_uninstall = e; g_has_pending_uninstall = true; }   // done next frame: the library list is rebuilt, `e` would dangle
    return removed;
}

/* The pill at the bottom while something installs: title, percentage, speed, time left, Cancel. Drawn on the foreground list (like the
 * toasts) with a mouse-only Cancel; it takes no pad focus, so controller navigation is untouched. Pad users cancel from the game's details. */
void draw_gamepack_pill() {
    if (GP.state != hubgp::Installer::Working) return;
    if (A.popup_last_frame) return;   // a popup is open: it would sit under the pill (the ghost details show their own progress)
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* fb = ui::F.bold;
    ImFont* fc = ui::F.cap;
    const float w = std::min(sp(92.f), io.DisplaySize.x * 0.9f), h = sp(8.5f);
    const ImVec2 p0((io.DisplaySize.x - w) * 0.5f, io.DisplaySize.y - sp(13.f) - (A.page == Page::Library ? sp(9.f) : 0.f) - h), p1(p0.x + w, p0.y + h);
    ui::shadow(dl, p0, p1, sp(2.f), 1.f);
    dl->AddRectFilled(p0, p1, ui::PANEL2, sp(2.f));
    dl->AddRect(p0, p1, ui::HAIRLINE, sp(2.f), 0, 1.f);
    const int ph = GP.phase;
    const float f = ph == hubgp::Installer::Preparing ? (GP.prep_total ? (float)((double)GP.prep_got / (double)GP.prep_total) : -1.f) : GP.fraction();
    std::string head = (ph == hubgp::Installer::Preparing ? "Preparing " : "Installing ") + GA.title;
    if (!GQ.empty()) head += "  (+" + std::to_string(GQ.size()) + " waiting)";
    if (GP.file_count > 1 && ph == hubgp::Installer::Downloading) head += "  -  file " + std::to_string(GP.file_idx.load()) + " of " + std::to_string(GP.file_count.load());
    const float cw = sp(11.f), ch = sp(3.75f);
    const ImVec2 c0(p1.x - sp(2.f) - cw, p0.y + sp(1.25f)), c1(c0.x + cw, c0.y + ch);
    dl->AddText(fb, fb->FontSize, ImVec2(p0.x + sp(2.f), p0.y + sp(1.5f)), ui::TEXT, ui::ellipsize(fb, fb->FontSize, head, c0.x - p0.x - sp(4.f)).c_str());
    const bool hov = io.MousePos.x >= c0.x && io.MousePos.x <= c1.x && io.MousePos.y >= c0.y && io.MousePos.y <= c1.y;
    dl->AddRectFilled(c0, c1, hov ? ui::scale_alpha(ui::BAD, 0.5f) : ui::PANEL, sp(1.f));
    dl->AddRect(c0, c1, ui::HAIRLINE, sp(1.f), 0, 1.f);
    ImVec2 ts = fc->CalcTextSizeA(fc->FontSize, FLT_MAX, 0.f, "Cancel");
    dl->AddText(fc, fc->FontSize, ImVec2(c0.x + (cw - ts.x) * 0.5f, c0.y + (ch - ts.y) * 0.5f), ui::TEXT, "Cancel");
    if (hov && ImGui::IsMouseClicked(0)) GP.request_cancel();
    const ImVec2 b0(p0.x + sp(2.f), p0.y + sp(4.5f)), b1(p1.x - sp(2.f), b0.y + sp(1.25f));
    dl->AddRectFilled(b0, b1, ui::PANEL, sp(0.6f));
    float fill = f;
    if (f < 0) fill = 0.15f;
    float x0 = b0.x, x1 = b0.x + (b1.x - b0.x) * std::min(1.f, fill);
    if (f < 0) { float t = (float)std::fmod(ImGui::GetTime() * 0.7, 1.0); x0 = b0.x + (b1.x - b0.x) * (t * 0.85f); x1 = x0 + (b1.x - b0.x) * 0.15f; }
    dl->AddRectFilled(ImVec2(x0, b0.y), ImVec2(x1, b1.y), ui::ACCENT, sp(0.6f));
    dl->AddText(fc, fc->FontSize, ImVec2(p0.x + sp(2.f), b1.y + sp(0.75f)), ui::TEXT2, ui::ellipsize(fc, fc->FontSize, gamepack_status_line(), w - sp(4.f)).c_str());
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
    if (std::getenv("HUB_SHOT_FAKE_DUPE_DS")) {   // hotfix shot: one friend lists Phantom Hourglass by its ROM header title, another by its proper title
        FR.f_libs["miauid"].push_back({"nds", "nds:azee-004d56", "", "Zelda_DS:PH"});
        FR.f_libs["jakuid"].push_back({"nds", "nds:fake-ph", "", "The Legend of Zelda: Phantom Hourglass"});
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

    if (g_headless && std::getenv("HUB_SHOT_GP_ENQUEUE") && now > 14.0) {   // screenshots: queue installs of the pack-only ghosts whose titles contain these (';'-separated), once the index is in
        static bool done = false;
        if (!done && !A.ghosts.empty()) {
            done = true;
            std::string list = std::getenv("HUB_SHOT_GP_ENQUEUE");
            size_t b = 0;
            while (b <= list.size()) {
                size_t e = list.find(';', b);
                std::string want = list.substr(b, e == std::string::npos ? std::string::npos : e - b);
                for (const auto& g : A.ghosts) if (g.uids.empty() && !want.empty() && g.title.find(want) != std::string::npos) { gamepack_enqueue(g.sys, g.title, g.id); break; }
                if (e == std::string::npos) break;
                b = e + 1;
            }
        }
    }

    /* my list -> the server. Signature = share flag + every game key; unchanged = nothing is ever sent. */
    if (!A.scanned_once || A.scanning) return;
    uint64_t h = 1469598103934665603ull;
    for (const hub::GameEntry& e : A.lib) {
        if (is_hack(e)) continue;
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
    SDL_Texture* t = hubart::get_ghost(g.sys, g.key, g.cid, g.title, hubart::Kind::Boxart, &w, &h, g.hint);
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

/* Installing / queued ghost: a progress bar along the cover's bottom edge (drawn after the title scrim so it stays visible). */
void ghost_tile_progress(ImDrawList* dl, const hubui::fg::Ghost& g, ImVec2 p0, ImVec2 p1) {
    float frac = -1.f;
    std::string lab;
    int st = gamepack_state_for(g.id, &frac, &lab);
    if (!st) return;
    float bh = sp(0.9f);
    ImVec2 b0(p0.x + sp(1.5f), p1.y - sp(0.75f) - bh), b1(p1.x - sp(1.5f), p1.y - sp(0.75f));
    dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 255, 70), bh * 0.5f);
    if (frac >= 0) { if (frac > 0) dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * std::min(1.f, frac), b1.y), ui::ACCENT, bh * 0.5f); }
    else if (st == 2) { float t = (float)std::fmod(ImGui::GetTime() * 0.8, 1.0); float x0 = b0.x + (b1.x - b0.x) * t * 0.8f; dl->AddRectFilled(ImVec2(x0, b0.y), ImVec2(x0 + (b1.x - b0.x) * 0.2f, b1.y), ui::ACCENT, bh * 0.5f); }
}

void ghost_tile_decor(ImDrawList* dl, const hubui::fg::Ghost& g, ImVec2 p0, ImVec2 p1, float e, std::string* subtitle) {
    /* desaturate: a neutral grey wash over the cover (lighter on hover so it is clear it can be opened) */
    dl->AddRectFilled(p0, p1, IM_COL32(96, 99, 108, (int)(150.f - 70.f * e)), sp(1.75f));
    std::vector<std::string> names = ghost_names(g);
    *subtitle = hubui::fg::badge_text(names);
    {   // installing / queued: the state as the subtitle (the bar itself is drawn on top of the title scrim: ghost_tile_progress)
        float frac = -1.f;
        std::string lab;
        if (gamepack_state_for(g.id, &frac, &lab)) *subtitle = lab;
    }
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

/* "1.4 GB download" for an installable game: asked of Drive once, in the background, when its details open. */
static std::string ghost_size_text(const hubui::fg::Ghost& g, bool want) {
    const std::string key = g.sys + "|" + g.title;
    if (SP.state == 2 && SP.th.joinable()) SP.th.join();
    if (!want) return "";
    if (SP.key == key && SP.state == 2) return SP.bytes ? hubgp::fmt_size(SP.bytes) + " download" : "";
    if (SP.state == 0 && SP.key != key) {
        SP.key = key;
        SP.state = 1;
        SP.bytes = 0;
        const std::string sys = g.sys, title = g.title;
        const std::vector<std::string> roots = hubgp::parse_links(A.st.pack_links);
        if (SP.th.joinable()) SP.th.join();
        SP.th = std::thread([sys, title, roots] { SP.bytes = hubgp::game_download_size(sys, title, roots); SP.state = 2; });
    } else if (SP.state == 2 && SP.key != key) { SP.state = 0; }
    return SP.key == key && SP.state == 1 ? "checking size..." : "";
}

void draw_ghost_modal() {
    g_ghost_popup_open = false;
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
    g_ghost_popup_open = true;
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

    /* install state of this game: can it be installed, is it queued / running, did the last try fail */
    float ifrac = -1.f;
    std::string ilab;
    const int ist_run = gamepack_state_for(g.id, &ifrac, &ilab);
    const bool builtin = hubgp::sys_folder(g.sys)[0] != '\0';
    const hubgp::IndexState ist = hubgp::index_state(g.sys);
    const bool can_show = builtin || (ist == hubgp::IndexState::Ready && hubgp::index_source(g.sys));
    const bool have = can_show && hubgp::installed(g.sys, g.title);
    bool in_pack = ist == hubgp::IndexState::Ready ? hubgp::index_has(g.sys, g.title) : true;   // unknown yet: checked live on click
    if (g_headless && std::getenv("HUB_SHOT_GHOST_INSTALLABLE")) in_pack = true;   // screenshots: pretend the shared pack has this game
    const bool installable = can_show && !have && in_pack && !ist_run;
    const bool reinstallable = can_show && have && !ist_run;   // files marked installed but no library entry exists (that is why this greyed tile is still here): wipe and install again
    const std::string size_txt = ghost_size_text(g, installable || ist_run);
    if (ist_run) {   // background job: this popup can be closed, the install goes on
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::ACCENT);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::PANEL);
        ImGui::ProgressBar(ifrac >= 0 ? ifrac : -1.0f * (float)ImGui::GetTime(), ImVec2(-1, sp(1.75f)), "");
        ImGui::PopStyleColor(2);
        text_wrapped_disabled(ist_run == 1 ? std::string("Waiting for the other installs to finish.") : gamepack_status_line());
        text_wrapped_disabled("You can close this and keep browsing. The install continues in the background.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    } else {
        if (GFAIL.count(g.id)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::BAD);
            ImGui::TextWrapped("Could not install: %s", GFAIL[g.id].c_str());
            ImGui::PopStyleColor();
        }
        if (installable && !size_txt.empty()) text_wrapped_disabled("Download size: " + size_txt + ". It is kept in RetroHub's games folder and shows up in your library right away.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }

    /* actions: Play / Play Online are shown but disabled (you cannot play what you do not have) */
    float bh = sp(5.5f);
    button("Play", ImVec2(sp(12.f), bh), false, false, ui::Icon::Play);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("You don't have this game");
    ImGui::SameLine(0, sp(1.5f));
    button("Play Online", ImVec2(sp(16.f), bh), false, false, ui::Icon::Globe);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("You don't have this game");
    if (can_show) {
        ImGui::SameLine(0, sp(1.5f));
        if (ist_run) {
            if (button("Cancel", ImVec2(sp(12.f), bh), false, true, ui::Icon::Close)) gamepack_cancel(g.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cancel this install");
        } else if (button(reinstallable ? "Reinstall" : GFAIL.count(g.id) ? "Retry" : "Install", ImVec2(sp(reinstallable ? 14.f : 12.f), bh), true, installable || reinstallable, ui::Icon::Cloud)) {
            if (reinstallable) hubgp::uninstall(g.sys, g.title);   // the old files never showed up as a game: start clean
            gamepack_enqueue(g.sys, g.title, g.id);   // the popup stays: it shows the progress and turns into the game's details when done
        }
        if (installable || reinstallable) ImGui::SetItemDefaultFocus();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (reinstallable) ImGui::SetTooltip("The files are in RetroHub's games folder but RetroHub does not list them as a game. Reinstall downloads them again.");
            else if (!in_pack) ImGui::SetTooltip("Not in any shared pack yet. The owner can add it to their Google Drive and send the link so everyone can install it.");
        }
    }
    ImGui::SameLine(0, sp(1.5f));
    if (button("Add your own copy", ImVec2(sp(20.f), bh), !installable && !ist_run, true, ui::Icon::Plus)) { ImGui::CloseCurrentPopup(); add_games_open(); }
    ImGui::SameLine(0, sp(1.5f));
    if (button("Close", ImVec2(sp(8.f), bh))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
