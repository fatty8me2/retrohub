#include "app.h"
#include "online_caps.h"

/* ---------- library / ROM scanning ---------- */


bool is_native(const hub::GameEntry& e) { return e.key == kNativeKey; }
std::string sys_short(const hub::GameEntry& e) { return hub::system_short(e.system); }
std::string no_slash(std::string p) {
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    return p;
}

void start_scan() {
    if (A.scanning) return;
    if (A.scan_th.joinable()) A.scan_th.join();
    A.scanning = true;
    A.scan_done = 0;
    A.scan_total = 0;
    std::vector<std::string> folders = A.st.rom_folders;
    A.scan_th = std::thread([folders]() {
        A.scan_result = hub::scan_library(folders, [](int d, int t) { A.scan_done = d; A.scan_total = t; });
        kt_scan_append(&A.scan_result, folders);   // Kanto Together package (zip / folder), if the user has it
        A.scan_ready = true;
    });
}

const hub::GameState& gstate(const std::string& key) {
    auto it = A.lib_state.find(key);
    if (it == A.lib_state.end()) it = A.lib_state.emplace(key, hub::game_state(key)).first;
    return it->second;
}
void refresh_state(const std::string& key) {
    A.lib_state[key] = hub::game_state(key);
    A.vis_dirty = true;
}
const hub::GameEntry* find_entry(const std::string& key) {
    auto it = A.lib_index.find(key);
    return it == A.lib_index.end() ? nullptr : &A.lib[it->second];
}

void apply_scan() {
    A.lib.clear();
    if (!smash_exe_path().empty()) {
        if (const GameInfo* g = hub::find_game("smash64")) {
            hub::GameEntry n;
            n.key = kNativeKey;
            n.system = hub::System::N64;
            n.title = g->title;
            n.serial = "";
            n.path = smash_exe_path();
            n.catalog_id = "smash64";
            A.lib.push_back(n);
        }
    }
    for (auto& e : A.scan_result) A.lib.push_back(e);
    if (const char* fl = std::getenv("HUB_SHOT_FAKE_LIB"); fl && g_headless) {   // screenshot/pad-nav aid: n made-up games, no scan result, no art, no finder
        static const hub::System sy[] = {hub::System::N64, hub::System::GBA, hub::System::GC, hub::System::N64, hub::System::GB};
        A.lib.clear();
        int n = std::max(1, std::atoi(fl));
        if (std::string(fl) == "all") {   // qa/reach_matrix.ps1: every system, an online catalog game + an uncatalogued one, plus Smash Classic, Remix and Kanto Together
            static const hub::System all[] = {hub::System::N64, hub::System::GB, hub::System::GBC, hub::System::GBA, hub::System::NDS,
                                              hub::System::N3DS, hub::System::GC, hub::System::WiiU, hub::System::PS1, hub::System::PS2};
            for (hub::System s : all) {
                std::string sh = hub::system_short(s);
                for (int pass = 0; pass < 2; ++pass) {
                    hub::GameEntry g;
                    g.system = s;
                    g.key = "fake:" + sh + (pass ? "-plain" : "-online");
                    g.path = "C:/fake/" + sh + (pass ? "-plain" : "-online") + ".rom";
                    if (pass == 0) {
                        const GameInfo* ci = nullptr;
                        for (const auto& c : hub::catalog()) if (c.system == sh && c.online && c.id != "smash64" && c.id != "remix" && c.id != kanto::kCatalogId) { ci = &c; break; }
                        if (!ci) continue;
                        g.catalog_id = ci->id;
                        g.title = ci->title;
                    } else {
                        g.title = std::string("Plain ") + hub::system_name(s) + " game";
                        if (s == hub::System::GC) g.serial = "GPLN01";   // a scanned disc always has its game ID
                    }
                    A.lib.push_back(g);
                }
            }
            for (const char* lid : {"pokered", "pokeemerald"})   // link-cable games
                if (const GameInfo* c = hub::find_game(lid)) { hub::GameEntry g; g.system = hub::system_from_short(c->system); g.key = std::string("fake:link-") + lid; g.title = c->title; g.catalog_id = lid; g.path = std::string("C:/fake/") + lid + ".rom"; A.lib.push_back(g); }
            if (const GameInfo* c = hub::find_game("remix")) { hub::GameEntry g; g.system = hub::System::N64; g.key = "fake:remix"; g.title = c->title; g.catalog_id = "remix"; g.path = "C:/fake/remix.z64"; A.lib.push_back(g); }
            { hub::GameEntry g; g.system = hub::System::GBA; g.key = kanto::kKey; g.title = kanto::kTitle; g.catalog_id = kanto::kCatalogId; g.issue = kanto::kIssue; g.path = "C:/fake/kanto.zip"; A.lib.push_back(g); }
            { hub::GameEntry g; g.system = hub::System::N64; g.key = kNativeKey; g.title = "Super Smash Bros. 64"; g.catalog_id = "smash64"; g.path = "C:/fake/BattleShip.exe"; A.lib.insert(A.lib.begin(), g); }
            n = 0;
        }
        for (int i = 0; i < n; ++i) {
            hub::GameEntry g;
            g.system = sy[i % 5];
            g.key = "fake:" + std::to_string(i);
            g.title = "Test Game " + std::to_string(i + 1);
            if (std::getenv("HUB_SHOT_FAKE_TITLES")) {   // picker screenshots: a big mixed library with real-looking names
                static const hub::System mix[] = {hub::System::N64, hub::System::GC, hub::System::PS1, hub::System::GBA, hub::System::NDS,
                                                  hub::System::N3DS, hub::System::GBC, hub::System::PS2, hub::System::WiiU, hub::System::GB};
                static const char* a1[] = {"Super", "Mega", "Turbo", "Legend of", "Mystic", "Dark", "Crystal", "Pocket", "Neon", "Royal"};
                static const char* a2[] = {"Kart", "Quest", "Arena", "Strike", "Tactics", "Racers", "Party", "Tennis", "Fighters", "Stadium"};
                static const char* a3[] = {"Remix", "Advance", "Deluxe", "World", "Rumble", "Legends", "Origins", "Zero", "Plus", "Blitz"};
                g.system = mix[i % 10];
                g.title = std::string(a1[i % 10]) + " " + a2[(i / 10) % 10] + " " + a3[(i / 7) % 10] + (i >= 100 ? " " + std::to_string(i / 100 + 1) : "");
            }
            g.path = "C:\fake\game" + std::to_string(i) + ".z64";
            A.lib.push_back(g);
        }
        {   // a catalog PlayStation game too, so the room picker / PS1 online path can be screenshotted (HUB_SHOT_ROOM_GAME=crashbash)
            hub::GameEntry g;
            g.system = hub::System::PS1;
            g.key = "ps1:scus-94570";
            g.catalog_id = "crashbash";
            g.serial = "SCUS-94570";
            g.title = "Crash Bash";
            g.path = "C:\fake\crashbash.cue";
            A.lib.push_back(g);
        }
        A.st.find_games_done = true;
        hubart::set_enabled(false);
    }
    A.lib_index.clear();
    A.lib_state.clear();
    if (std::getenv("HUB_SHOT_FAKE_TITLES") && g_headless) {   // picker screenshots: some favourites and recently played games
        for (size_t i = 0; i < A.lib.size(); ++i) {
            hub::GameState gs;
            if (i % 11 == 3) gs.favorite = true;
            if (i % 13 == 5) gs.last_played = 1760000000ull + i * 3600;
            if (gs.favorite || gs.last_played) A.lib_state[A.lib[i].key] = gs;
        }
    }
    for (size_t i = 0; i < A.lib.size(); ++i) A.lib_index.emplace(A.lib[i].key, (int)i);   // first wins on duplicates
    A.vis_dirty = true;
    A.hello_dirty = true;
    A.info_at = -100;
    A.scanned_once = true;
}

/* Curated catalog entry for a scanned game (id/MD5 first, then serial + system). */
const GameInfo* catalog_for(const hub::GameEntry& e) {
    if (!e.catalog_id.empty())
        if (const GameInfo* g = hub::find_game(e.catalog_id)) return g;
    std::string md5;
    if (e.key.compare(0, 4, "n64:") == 0)
        for (char c : e.key.substr(4)) md5 += (char)std::toupper((unsigned char)c);
    return hub::find_game_for(sys_short(e), e.serial, md5);
}

/* "Recently added": when the game file appeared on disk (creation time, else last write); cached per path. */
static unsigned long long added_unix(const std::string& path) {
    static std::map<std::string, unsigned long long> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    unsigned long long v = 0;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &d)) {
        ULARGE_INTEGER c, w;
        c.LowPart = d.ftCreationTime.dwLowDateTime; c.HighPart = d.ftCreationTime.dwHighDateTime;
        w.LowPart = d.ftLastWriteTime.dwLowDateTime; w.HighPart = d.ftLastWriteTime.dwHighDateTime;
        ULONGLONG t = c.QuadPart && c.QuadPart < w.QuadPart ? c.QuadPart : w.QuadPart;
        if (t > 116444736000000000ull) v = (t - 116444736000000000ull) / 10000000ull;
    }
    cache[path] = v;
    return v;
}

void rebuild_vis() {
    A.sort_mode = hubui::sort_from_key(A.st.library_sort);
    rebuild_ghosts();   // friends' games you do not own (index >= lib.size() in items / vis / shelves)
    std::vector<hubui::ListItem> items;
    items.reserve(A.lib.size() + A.ghosts.size());
    for (auto& e : A.lib) { const hub::GameState& g0 = gstate(e.key); items.push_back({e.key, e.title, sys_short(e), is_native(e), g0.last_played, g0.play_seconds, added_unix(e.path)}); }
    for (auto& g : A.ghosts) { hubui::ListItem li{"ghost:" + g.id, g.title, g.sys, false, 0, 0, 0}; li.ghost = true; items.push_back(li); }
    std::string q = A.search_buf;
    std::vector<int> base = hubui::filter_sort(items, "", q, A.sort_mode);   // search applied, all consoles
    if (A.online_only) {
        std::vector<int> keep;
        for (int i : base) {
            if (i >= (int)A.lib.size()) {   // a friend's game: online-capable when the catalog says so
                const hubui::fg::Ghost& g = A.ghosts[i - A.lib.size()];
                const GameInfo* gc = g.cid.empty() ? nullptr : hub::find_game(g.cid);
                if (gc && gc->online) keep.push_back(i);
                continue;
            }
            const hub::GameEntry& e = A.lib[i];
            const GameInfo* c = catalog_for(e);
            if (is_native(e) || (c && c->online)) keep.push_back(i);
        }
        base.swap(keep);
    }
    A.chip_counts.clear();
    A.chip_total = (int)base.size();
    A.vis.clear();
    for (int i : base) {
        ++A.chip_counts[items[i].system];
        if (A.sys_filter.empty() || items[i].system == A.sys_filter) A.vis.push_back(i);
    }
    A.secs = hubui::group_by_console(items, &A.vis, A.sort_mode);

    // Home composition: shelves over the whole library, plus catalog games the player doesn't own.
    A.sg.clear();
    std::set<std::string> owned_ids;
    for (auto& e : A.lib) {
        const GameInfo* cat = catalog_for(e);
        if (cat) owned_ids.insert(cat->id);
        const hub::GameState& gs = gstate(e.key);
        hubui::ShelfGame g;
        g.key = e.key;
        g.title = e.title;
        g.system = sys_short(e);
        g.favorite = gs.favorite;
        g.last_played = gs.last_played;
        g.online = is_native(e) || (cat && cat->online);
        g.pinned = is_native(e);
        g.play_seconds = gs.play_seconds;
        g.added = added_unix(e.path);
        A.sg.push_back(g);
    }
    for (auto& gh : A.ghosts) {
        hubui::ShelfGame g;
        g.key = "ghost:" + gh.id;
        g.title = gh.title;
        g.system = gh.sys;
        g.ghost = true;
        A.sg.push_back(g);
    }
    A.shelves = hubui::compose_shelves(A.sg, 12, A.sort_mode);
    const std::vector<GameInfo>& cat = hub::catalog();
    std::vector<bool> owned(cat.size()), online(cat.size());
    for (size_t i = 0; i < cat.size(); ++i) {
        owned[i] = cat[i].engine == hub::Engine::Native || owned_ids.count(cat[i].id) > 0 || cat[i].id == kanto::kCatalogId;   // never a top pick: it comes from a friend's package
        online[i] = cat[i].online;
    }
    A.pick_idx = hubui::pick_unowned(owned, online, 16);
    A.vis_filter = A.sys_filter;
    A.vis_query = q;
    A.vis_dirty = false;
}

/* ---------- online capability ---------- */

bool supports_online(const hub::GameEntry& e) {
    /* hubcaps::online_capable: every game of N64/GB/GBC/GBA/3DS/GC (Dolphin NetPlay, any disc)/NDS (ds-link)/PS1 (rollback, multitap for 3-4).
     * Non-catalog GC/NDS/PS1 games are "untested" (online_untested). PS2 and Wii U stay out. Unit-tested against launch_online's branches. */
    return hubcaps::online_capable(e.system, hub::find_game(e.catalog_id));
}
/* Capable but not vetted (GC/DS/PS1 outside the catalog's online list): the picker tags it "Untested online". */
bool online_untested(const hub::GameEntry& e) { return hubcaps::online_untested(e.system, hub::find_game(e.catalog_id)); }
/* Online player cap for a room (online_max_for; 0 online_max = max_players). Every online limit uses this. */
int max_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return hub::online_max_for(*g);
    return hubcaps::default_online_max(e.system);   // not in the catalog: GB/GBC/DS/PS1 2, the rest 4
}
/* Local player count shown on cards (not the online cap). */
int local_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return g->max_players;
    return max_players_for(e);
}
/* How a game is named in lobby messages: the N64 catalog id (back-compat) or the library key. */
std::string room_id(const hub::GameEntry& e) { return e.catalog_id.empty() ? e.key : e.catalog_id; }
const hub::GameEntry* find_room_entry(const std::string& id) {
    for (auto& e : A.lib)
        if (supports_online(e) && room_id(e) == id) return &e;
    return nullptr;
}
std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
void my_games(std::map<std::string, std::string>* games, std::vector<std::string>* keys) {
    for (auto& e : A.lib) {
        if (!supports_online(e)) continue;
        if (is_native(e)) { (*games)["smash64"] = "native"; continue; }
        if (!e.catalog_id.empty() && e.key.compare(0, 4, "n64:") == 0 && !games->count(e.catalog_id))
            (*games)[e.catalog_id] = upper(e.key.substr(4));
        if (keys) {
            keys->push_back(e.key);
            /* The room names a catalog game by its catalog id (room_id), so advertise that too, or game_selectable never finds non-N64 catalog games (GameCube) */
            if (!e.catalog_id.empty() && e.key.compare(0, 4, "n64:") != 0 && std::find(keys->begin(), keys->end(), e.catalog_id) == keys->end()) keys->push_back(e.catalog_id);
        }
    }
}

