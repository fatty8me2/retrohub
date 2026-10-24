#include "app.h"

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
        for (int i = 0; i < n; ++i) {
            hub::GameEntry g;
            g.system = sy[i % 5];
            g.key = "fake:" + std::to_string(i);
            g.title = "Test Game " + std::to_string(i + 1);
            g.path = "C:\fake\game" + std::to_string(i) + ".z64";
            A.lib.push_back(g);
        }
        A.st.find_games_done = true;
        hubart::set_enabled(false);
    }
    A.lib_index.clear();
    A.lib_state.clear();
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

void rebuild_vis() {
    std::vector<hubui::ListItem> items;
    items.reserve(A.lib.size());
    for (auto& e : A.lib) items.push_back({e.key, e.title, sys_short(e), is_native(e)});
    std::string q = A.search_buf;
    std::vector<int> base = hubui::filter_sort(items, "", q);   // search applied, all consoles
    if (A.online_only) {
        std::vector<int> keep;
        for (int i : base) {
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
    A.secs = hubui::group_by_console(items, &A.vis);

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
        A.sg.push_back(g);
    }
    A.shelves = hubui::compose_shelves(A.sg);
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
    if (e.system == hub::System::GC) {   // Dolphin NetPlay (netplay_dolphin.h); only catalog games flagged online
        const GameInfo* g = hub::find_game(e.catalog_id);
        return g && g->online;
    }
    if (e.system == hub::System::NDS) {   // local wireless (trades, battles, Union Room) through retrorun --ds-link; catalog games only
        const GameInfo* g = hub::find_game(e.catalog_id);
        return g && g->online;
    }
    return e.system == hub::System::N64 || e.system == hub::System::GBA || e.system == hub::System::GB ||
           e.system == hub::System::GBC || e.system == hub::System::N3DS;   // 3DS: Azahar room via np_3ds
}
/* Online player cap for a room (online_max_for; 0 online_max = max_players). Every online limit uses this. */
int max_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return hub::online_max_for(*g);
    return (e.system == hub::System::GB || e.system == hub::System::GBC) ? 2 : 4;
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

