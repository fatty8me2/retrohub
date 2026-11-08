#include "app.h"
#include "wave3_hooks.h"
#include <ctime>
#include "library_scan_info.h"
#include "online_caps.h"
#include "gameinfo.h"
#include "gamepack.h"
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

/* ---------- library / ROM scanning ---------- */


bool is_native(const hub::GameEntry& e) { return e.key == kNativeKey; }
std::string sys_short(const hub::GameEntry& e) { return hub::system_short(e.system); }
std::string no_slash(std::string p) {
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    return p;
}

static bool g_scan_again = false;   // a scan was asked for while one ran: run it when that one lands

void start_scan() {
    hubgp::request_recheck();   // a rescan also looks for new games in the shared Drive packs
    if (A.scanning) { g_scan_again = true; return; }   // never dropped: the running scan may have started before the new game's folder existed
    if (A.scan_th.joinable()) A.scan_th.join();
    A.scanning = true;
    A.scan_done = 0;
    A.scan_total = 0;
    std::vector<std::string> folders = A.st.rom_folders;
    A.scan_th = std::thread([folders]() {
        A.scan_result = hub::scan_library(folders, [](int d, int t) { A.scan_done = d; A.scan_total = t; });
        kt_scan_append(&A.scan_result, folders);   // Kanto Together package (zip / folder), if the user has it
        hack_scan_append(&A.scan_result, folders); // ROM hacks / translations whose base game is in the scan (Plan 4 H2)
        {   // library-1: the other copies of each game come last, so the listed (preferred) copy stays first and hubdupes::collapse sees every path
            std::vector<hub::GameEntry> extra = hub::scan_extra_copies();
            A.scan_result.insert(A.scan_result.end(), extra.begin(), extra.end());
        }
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
    lib_lookup_cache_clear();
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
    {   /* a game installed from a shared pack keeps the pack's title: a disc archive's inner file name ("game.iso") or a header title
         * ("CONTROLGBA") would otherwise differ from the greyed-out entry it replaces, and the ghost would never go away */
        const std::string root = hubgp::pack_root();
        for (auto& e : A.scan_result) {
            if (e.catalog_id.empty()) {
                std::string t = hubgp::installed_folder_title(e.path, root);
                if (!t.empty() && hubgp::pack_key(t) != hubgp::pack_key(e.title)) e.title = t;
            }
            A.lib.push_back(e);
        }
        gamepack_merge_added(&A.lib);   // games installed this session that an older scan result does not hold yet
    }
    {   // prefs: hidden games drop out; duplicates collapse to one copy (the preferred path if it was found, else the first scanned)
        std::map<std::string, std::string> pref = preferred_copies();
        std::set<std::string> hid = hidden_game_keys();
        if (!hid.empty())
            A.lib.erase(std::remove_if(A.lib.begin(), A.lib.end(), [&](const hub::GameEntry& g) { return g.key != kNativeKey && hid.count(g.key) > 0; }), A.lib.end());
        hubdupes::collapse(&A.lib, pref, &A.dupes);
        apply_located_paths(&A.lib, pref);
    }
    if (const char* fl = std::getenv("HUB_SHOT_FAKE_LIB"); fl && g_headless) {   // screenshot/pad-nav aid: n made-up games, no scan result, no art, no finder
        static const hub::System sy[] = {hub::System::N64, hub::System::GBA, hub::System::GC, hub::System::N64, hub::System::GB};
        A.lib.clear();
        int n = std::max(1, std::atoi(fl));
        const bool only_empty = std::string(fl) == "empty";   // QA: a truly empty own library (no bundled game either)
        const bool only_native = std::string(fl) == "native" || only_empty;   // QA: just the bundled Smash 64 entry (a fresh install with no ROM folders)
        if (only_empty) n = 0;
        else if (only_native) { hub::GameEntry g; g.system = hub::System::N64; g.key = kNativeKey; g.title = "Super Smash Bros. 64"; g.catalog_id = "smash64"; g.path = "C:/fake/BattleShip.exe"; A.lib.push_back(g); n = 0; }
        if (std::string(fl) == "all") {   // qa/reach_matrix.ps1: every system, an online catalog game + an uncatalogued one, plus Smash Classic, Remix and Kanto Together
            static const hub::System all[] = {hub::System::N64, hub::System::GB, hub::System::GBC, hub::System::GBA, hub::System::NDS,
                                              hub::System::N3DS, hub::System::GC, hub::System::WiiU, hub::System::PS1, hub::System::PS2, hub::System::NES, hub::System::SNES};
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
        if (std::string(fl) == "tpak") {   // Wave 3 X1 screenshot aid: Pokemon Stadium 2 + a few Game Boy games (HUB_SHOT_DETAILS=fake:tpak-n64)
            struct F { const char* key; hub::System s; const char* title; const char* cat; };
            for (const F& f : {F{"fake:tpak-n64", hub::System::N64, "Pokemon Stadium 2", "pokestadium2"}, F{"fake:tpak-gold", hub::System::GBC, "Pokemon Gold", "pokegold"},
                               F{"fake:tpak-crystal", hub::System::GBC, "Pokemon Crystal", "pokecrystal"}, F{"fake:tpak-red", hub::System::GB, "Pokemon Red", "pokered"}}) {
                hub::GameEntry g; g.system = f.s; g.key = f.key; g.title = f.title; g.catalog_id = f.cat; g.path = std::string("C:/fake/") + f.cat + ".rom";
                A.lib.push_back(g);
            }
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
            if (std::getenv("HUB_SHOT_FAKE_AZ")) {   // library-layout screenshots: titles spread over A-Z (and a few digits), mixed consoles
                static const hub::System mix2[] = {hub::System::N64, hub::System::GC, hub::System::PS1, hub::System::GBA, hub::System::NDS,
                                                   hub::System::N3DS, hub::System::GBC, hub::System::PS2, hub::System::WiiU, hub::System::GB};
                static const char* w1[] = {"Aero", "Blast", "Cosmic", "Dragon", "Elite", "Fury", "Galaxy", "Hyper", "Iron", "Jungle", "Kart", "Legend", "Mystic",
                                           "Neon", "Omega", "Pixel", "Quest", "Rogue", "Star", "Turbo", "Ultra", "Vortex", "Wild", "Xeno", "Yoshi", "Zero"};
                static const char* w2[] = {"Racers", "Knights", "Tactics", "Arena", "Odyssey", "Rumble", "Kingdom", "Stadium", "Legends", "Warriors"};
                g.system = mix2[(i * 7) % 10];
                g.title = i % 40 == 39 ? std::to_string(1000 + i) + " Club" : std::string(w1[(i * 5 + i / 26) % 26]) + " " + w2[(i / 3) % 10] + (i >= 260 ? " " + std::to_string(i / 260 + 1) : "");
            }
            if (std::getenv("HUB_SHOT_FAKE_SHELF")) {   // Shelf screenshots: every console, real-looking and a few very long titles (HUB_SHOT_FAKE_SHELF=<system short> keeps one console)
                static const hub::System all13[] = {hub::System::NES, hub::System::SNES, hub::System::N64, hub::System::GB, hub::System::GBC, hub::System::GBA, hub::System::NDS,
                                                    hub::System::N3DS, hub::System::PS1, hub::System::PS2, hub::System::GC, hub::System::WiiU, hub::System::PSP};
                static const char* w1[] = {"Super", "Mega", "Turbo", "Legend of", "Mystic", "Dark", "Crystal", "Pocket", "Neon", "Royal", "Kirby's", "Star", "Metroid"};
                static const char* w2[] = {"Kart", "Quest", "Arena", "Strike", "Tactics", "Racers", "Party", "Tennis", "Fighters", "Stadium", "Odyssey", "Warriors"};
                static const char* w3[] = {"Remix", "Advance", "Deluxe", "World", "Rumble", "Legends", "Origins", "Zero", "Plus", "Blitz"};
                const char* only = std::getenv("HUB_SHOT_FAKE_SHELF");
                g.system = all13[(i * 5 + i / 13) % 13];
                if (only && only[0] && only[0] != '1') g.system = hub::system_from_short(only);
                g.title = std::string(w1[(i * 3) % 13]) + " " + w2[(i * 7 + i / 5) % 12] + " " + w3[(i / 3) % 10];
                if (i % 11 == 4) g.title += ": The Adventures of the Forgotten Kingdom Collector's Edition";
                if (i % 17 == 9) g.title = "Q" + g.title;
            }
            g.path = "C:/fake/game" + std::to_string(i) + ".z64";
            A.lib.push_back(g);
        }
        if (!only_native) {   // a catalog PlayStation game too, so the room picker / PS1 online path can be screenshotted (HUB_SHOT_ROOM_GAME=crashbash)
            hub::GameEntry g;
            g.system = hub::System::PS1;
            g.key = "ps1:scus-94570";
            g.catalog_id = "crashbash";
            g.serial = "SCUS-94570";
            g.title = "Crash Bash";
            g.path = "C:/fake/crashbash.cue";
            A.lib.push_back(g);
        }
        if (!only_native && std::getenv("HUB_SHOT_SWITCH")) {   // Switch shelf / details screenshots (qa: HUB_SHOT_SWITCH=1); the last one is a Switch 2 game
            static const char* ids[] = {"0100000000010000", "01007ef00011e000", "0400000000a1b000"};
            static const char* names[] = {"Super Mario Odyssey", "The Legend of Zelda: Breath of the Wild", "Mario Kart World"};
            for (int i = 0; i < 3; ++i) {
                hub::GameEntry g;
                g.system = hub::System::Switch;
                g.key = std::string("switch:") + ids[i];
                g.serial = ids[i];
                g.title = names[i];
                g.nx2 = i == 2;
                g.path = std::string("C:/fake/") + ids[i] + ".nsp";
                A.lib.push_back(g);
            }
        }
        if (!only_native && g_headless && std::getenv("HUB_SHOT_HOOKSHOT")) {   // Hookshot card screenshots (hookshot_ui.h): Ocarina of Time (Switch 2)
            hub::GameEntry g;
            g.system = hub::System::Switch;
            g.key = "switch:0400486024dec000";
            g.serial = "0400486024DEC000";
            g.title = "The Legend of Zelda: Ocarina of Time";
            g.nx2 = true;
            g.path = "C:/fake/0400486024dec000.nup";
            A.lib.push_back(g);
        }
        if (!only_native && std::getenv("HUB_SHOT_PSP")) {   // PSP shelf / details screenshots (qa: HUB_SHOT_PSP=1)
            static const char* ids[] = {"ulus10041", "ucus98632", "npuz00001"};
            static const char* names[] = {"Crisis Core: Final Fantasy VII", "God of War: Chains of Olympus", "Echochrome"};
            for (int i = 0; i < 3; ++i) {
                hub::GameEntry g;
                g.system = hub::System::PSP;
                g.key = std::string("psp:") + ids[i];
                g.serial = ids[i];
                g.title = names[i];
                g.path = std::string("C:/fake/") + ids[i] + ".iso";
                A.lib.push_back(g);
            }
        }
        if (!only_native && !std::getenv("HUB_SHOT_NO_WIIU")) {   // a catalog Wii U remote-play game (HUB_SHOT_ROOM_GAME=sm3dw); HUB_SHOT_NO_WIIU = this PC has no copy (guest view)
            hub::GameEntry g;
            g.system = hub::System::WiiU;
            g.key = "wiiu:0005000010145d00";
            g.catalog_id = "sm3dw";
            g.serial = "0005000010145D00";
            g.title = "Super Mario 3D World";
            g.path = "C:/fake/sm3dw.rpx";
            A.lib.push_back(g);
        }
        if (std::getenv("HUB_SHOT_FAKE_DUPES") && A.lib.size() > 2) {   // details > About shows "3 copies found"
            A.dupes.clear();
            A.dupes[A.lib[2].key] = {A.lib[2].path, "D:/Roms/N64 backup/" + A.lib[2].title + ".z64", "E:/Old PC/Games/" + A.lib[2].title + " (copy).z64"};
        }
        if (std::getenv("HUB_SHOT_HACKS") && A.lib.size() >= 2) {   // screenshot aid: two hacks under the first fake game (keys ending 0 = hack, 1 = translation)
            const hub::GameEntry base = A.lib[0];
            for (int i = 1; i >= 0; --i) {
                hub::GameEntry h = base;
                h.key = base.key + "+h0000000" + std::to_string(i);
                h.title = i ? base.title + " (English)" : base.title + " Kaizo";
                h.catalog_id.clear();
                h.hack_patch = "C:/fake/patch.bps";
                h.hack_base = base.key;
                A.lib.insert(A.lib.begin() + 1, h);
            }
        }
        gameinfo_ui_shot_lib();
        palpark_shot_init();   // Wave 3 X3: HUB_SHOT_PALPARK=1|none (with HUB_SHOT_FAKE_LIB=1)
        A.st.find_games_done = true;
        hubart::set_enabled(false);
    }
    A.lib_index.clear();
    A.lib_state.clear();
    if (std::getenv("HUB_SHOT_FAKE_AZ") && g_headless) {   // library-layout screenshots: favourites, last played and play time
        for (size_t i = 0; i < A.lib.size(); ++i) {
            hub::GameState gs;
            if (i % 9 == 2) gs.favorite = true;
            if (i % 4 == 1) { gs.last_played = (unsigned long long)std::time(nullptr) - (i % 40) * 86400ull - 3600; gs.play_seconds = 600 + (i * 977) % 90000; }
            if (gs.favorite || gs.last_played) A.lib_state[A.lib[i].key] = gs;
        }
    }
    if (std::getenv("HUB_SHOT_FAKE_TITLES") && g_headless) {   // picker screenshots: some favourites and recently played games
        for (size_t i = 0; i < A.lib.size(); ++i) {
            hub::GameState gs;
            if (i % 11 == 3) gs.favorite = true;
            if (i % 13 == 5) gs.last_played = 1760000000ull + i * 3600;
            if (gs.favorite || gs.last_played) A.lib_state[A.lib[i].key] = gs;
        }
    }
    custom_reset_originals();   // user renames (synced prefs) replace the scanned titles in A.lib; the scanned ones are kept for lookups + search
    for (size_t i = 0; i < A.lib.size(); ++i) A.lib_index.emplace(A.lib[i].key, (int)i);   // first wins on duplicates
    A.vis_dirty = true;
    A.hello_dirty = true;
    A.info_at = -100;
    A.scanned_once = true;
    if (g_scan_again) { g_scan_again = false; start_scan(); }
}

/* Curated catalog entry for a scanned game (id/MD5 first, then serial + system). */
static const GameInfo* catalog_for_uncached(const hub::GameEntry& e) {
    if (!e.catalog_id.empty())
        if (const GameInfo* g = hub::find_game(e.catalog_id)) return g;
    std::string md5;
    if (e.key.compare(0, 4, "n64:") == 0)
        for (char c : e.key.substr(4)) md5 += (char)std::toupper((unsigned char)c);
    return hub::find_game_for(sys_short(e), e.serial, md5);
}

/* Per-frame callers (every tile, the Online chip) must not redo the catalog scans: results are memoized on a hash of the fields the lookup and
 * the online rule read (so a rename or a rescan that changes them just misses), and dropped on every rescan. The catalog itself is a static list. */
namespace {
std::mutex g_memo_mu;
std::unordered_map<unsigned long long, const GameInfo*> g_cat_memo;
std::unordered_map<unsigned long long, bool> g_online_memo;
std::unordered_map<unsigned long long, int> g_year_memo;
unsigned long long hmix(unsigned long long h, const std::string& s) { return (h ^ std::hash<std::string>()(s)) * 1099511628211ull + 0x9e3779b97f4a7c15ull; }
unsigned long long entry_hash(const hub::GameEntry& e, bool with_title) {
    unsigned long long h = 1469598103934665603ull + (unsigned long long)e.system;
    h = hmix(h, e.catalog_id);
    h = hmix(h, e.key);
    h = hmix(h, e.serial);
    if (with_title) h = hmix(h, e.title);
    return h;
}
}  // namespace

void lib_lookup_cache_clear() {
    std::lock_guard<std::mutex> l(g_memo_mu);
    g_cat_memo.clear();
    g_online_memo.clear();
    g_year_memo.clear();
}

const GameInfo* catalog_for(const hub::GameEntry& e) {
    const unsigned long long h = entry_hash(e, false);
    {
        std::lock_guard<std::mutex> l(g_memo_mu);
        auto it = g_cat_memo.find(h);
        if (it != g_cat_memo.end()) return it->second;
    }
    const GameInfo* g = catalog_for_uncached(e);
    std::lock_guard<std::mutex> l(g_memo_mu);
    if (g_cat_memo.size() > 50000) g_cat_memo.clear();
    g_cat_memo[h] = g;
    return g;
}

/* Release year for sorting and the carousel's year marks: the same source as the subtitle under the centre case (the bundled game database,
 * then the curated catalog). Memoized per game (a rename or rescan that changes the hashed fields just misses). */
int lib_year_of(int idx) {
    if (idx < 0) return 0;
    if (idx >= (int)A.lib.size()) {
        const size_t gi = (size_t)idx - A.lib.size();
        if (gi >= A.ghosts.size()) return 0;
        const GameInfo* gc = A.ghosts[gi].cid.empty() ? nullptr : hub::find_game(A.ghosts[gi].cid);
        return gc ? gc->year : 0;
    }
    const hub::GameEntry& e = A.lib[(size_t)idx];
    const unsigned long long h = entry_hash(e, true);
    {
        std::lock_guard<std::mutex> l(g_memo_mu);
        auto it = g_year_memo.find(h);
        if (it != g_year_memo.end()) return it->second;
    }
    const GameInfo* cat = catalog_for(e);
    CaseInfo ci;
    gameinfo_case_info(e, cat ? cat->title : std::string(), false, &ci);
    int y = ci.year > 0 ? ci.year : (cat ? cat->year : 0);
    if (y <= 0 && g_headless && e.key.compare(0, 5, "fake:") == 0 && std::getenv("HUB_SHOT_FAKE_LIB")) {   // screenshot aid: made-up years so the year marks have something to show
        unsigned h2 = 2166136261u;
        for (char c : e.key) h2 = (h2 ^ (unsigned char)c) * 16777619u;
        y = (h2 % 11u) == 0 ? 0 : 1984 + (int)((h2 >> 8) % 36u);
    }
    std::lock_guard<std::mutex> l(g_memo_mu);
    if (g_year_memo.size() > 50000) g_year_memo.clear();
    g_year_memo[h] = y;
    return y;
}

/* The Online chip / badge rule: the native port, or any game that can be played in a room. */
bool entry_online(const hub::GameEntry& e) {
    if (is_native(e)) return true;
    const unsigned long long h = entry_hash(e, true);
    {
        std::lock_guard<std::mutex> l(g_memo_mu);
        auto it = g_online_memo.find(h);
        if (it != g_online_memo.end()) return it->second;
    }
    const bool on = supports_online(e);
    std::lock_guard<std::mutex> l(g_memo_mu);
    if (g_online_memo.size() > 50000) g_online_memo.clear();
    g_online_memo[h] = on;
    return on;
}

/* "Recently added": when the game file appeared on disk (creation time, else last write); cached per path. */
static unsigned long long stat_added(const std::string& path) {
    unsigned long long v = 0;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &d)) {
        ULARGE_INTEGER c, w;
        c.LowPart = d.ftCreationTime.dwLowDateTime; c.HighPart = d.ftCreationTime.dwHighDateTime;
        w.LowPart = d.ftLastWriteTime.dwLowDateTime; w.HighPart = d.ftLastWriteTime.dwHighDateTime;
        ULONGLONG t = c.QuadPart && c.QuadPart < w.QuadPart ? c.QuadPart : w.QuadPart;
        if (t > 116444736000000000ull) v = (t - 116444736000000000ull) / 10000000ull;
    }
    return v;
}

/* The stat can be slow (network drives), so it never runs on the UI thread: an unknown path reports 0 and is
   queued for one worker; when the batch lands added_stamps_changed() turns true once so the list is rebuilt. */
namespace {
std::mutex g_added_mu;
std::map<std::string, unsigned long long> g_added_cache;
std::deque<std::string> g_added_queue;
std::set<std::string> g_added_queued;
bool g_added_running = false, g_added_changed = false;

void added_worker() {
    for (;;) {
        std::string p;
        if (g_io_shutdown) return;
        {
            std::lock_guard<std::mutex> l(g_added_mu);
            if (g_added_queue.empty()) { g_added_running = false; return; }
            p = std::move(g_added_queue.front());
            g_added_queue.pop_front();
        }
        unsigned long long v = stat_added(p);
        if (g_io_shutdown) return;   // app is tearing down: leave the (soon destroyed) statics alone
        std::lock_guard<std::mutex> l(g_added_mu);
        g_added_cache[p] = v;
        g_added_queued.erase(p);
        if (g_added_queue.empty()) g_added_changed = true;   // whole batch done: one rebuild
    }
}
}

bool added_stamps_changed() {
    std::lock_guard<std::mutex> l(g_added_mu);
    bool c = g_added_changed;
    g_added_changed = false;
    return c;
}

static unsigned long long added_unix(const std::string& path) {
    std::lock_guard<std::mutex> l(g_added_mu);
    auto it = g_added_cache.find(path);
    if (it != g_added_cache.end()) return it->second;
    if (g_headless) { unsigned long long v = stat_added(path); g_added_cache[path] = v; return v; }   // screenshots stay deterministic
    if (g_added_queued.insert(path).second) {
        g_added_queue.push_back(path);
        if (!g_added_running) { g_added_running = true; std::thread(added_worker).detach(); }
    }
    return 0;
}

unsigned long long lib_added_unix(const std::string& path) { return added_unix(path); }   // the Cases scrubber's "added" marks

static std::string genre_lookup(const gameinfo::Query& q) {
    std::string g;
    if (!q.system.empty()) g = gameinfo::shared_db(hub::exe_dir() + "data/").lookup(q).genre;
    size_t cut = g.find_first_of(",;/");
    if (cut != std::string::npos) g.resize(cut);
    size_t b = g.find_first_not_of(' ');
    g = b == std::string::npos ? std::string() : g.substr(b);
    while (!g.empty() && g.back() == ' ') g.pop_back();
    return g.empty() ? std::string("Other") : g;
}

/* Called from rebuild_vis (UI thread); the mutex keeps the caches safe if anything else ever asks. */
std::string genre_of_entry(const hub::GameEntry& e) {
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(e.key);
    if (it != cache.end()) return it->second;
    gameinfo::Query q;
    if (e.catalog_id != "remix" && e.catalog_id != "kanto") {
        q.system = hub::system_short(e.system);
        q.title = custom_original_title(e);
        q.serial = e.serial;
        if (e.catalog_id == "smash64") q.alt_title = "Super Smash Bros.";
        else if (const GameInfo* c = catalog_for(e)) q.alt_title = c->title;
    }
    return cache[e.key] = genre_lookup(q);
}

/* A friend's game you do not own: the same lookup through its catalog title, so filter and grouping treat it like owned games. */
std::string genre_of_ghost(const hubui::fg::Ghost& g) {
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(g.id);
    if (it != cache.end()) return it->second;
    gameinfo::Query q;
    q.system = g.sys;
    q.title = g.title;
    if (const GameInfo* c = g.cid.empty() ? nullptr : hub::find_game(g.cid)) q.alt_title = c->title;
    return cache[g.id] = genre_lookup(q);
}

static std::string genre_of_index(int i) {
    return i < (int)A.lib.size() ? genre_of_entry(A.lib[i]) : genre_of_ghost(A.ghosts[i - A.lib.size()]);
}

void rebuild_vis() {
    A.sort_mode = hubui::sort_from_key(A.st.library_sort);
    A.sort_rev = A.st.library_sort_rev;
    A.sort_favs = A.st.library_favs_first;
    const hubui::SortSpec spec(A.sort_mode, A.sort_rev, A.sort_favs);
    const bool need_year = A.sort_mode == hubui::SortMode::ReleaseYear;
    rebuild_ghosts();   // friends' games you do not own (index >= lib.size() in items / vis / shelves)
    std::vector<hubui::ListItem> items;
    items.reserve(A.lib.size() + A.ghosts.size());
    for (auto& e : A.lib) {
        const hub::GameState& g0 = gstate(e.key);
        hubui::ListItem li{e.key, e.title, sys_short(e), is_native(e), g0.last_played, g0.play_seconds, added_unix(e.path)};
        li.favorite = g0.favorite;
        if (need_year) li.year = lib_year_of((int)items.size());
        if (custom_is_renamed(e.key)) li.alt_title = custom_original_title(e);   // search also matches the scanned name
        hubui::prepare_sort_key(li);   // lower-cased title, "#" block and console rank once per build, not per comparison
        items.push_back(li);
    }
    for (auto& g : A.ghosts) { hubui::ListItem li{"ghost:" + g.id, g.title, g.sys, false, 0, 0, 0}; li.ghost = true; hubui::prepare_sort_key(li); if (need_year) li.year = lib_year_of((int)items.size()); items.push_back(li); }
    std::string q = A.search_buf;
    std::vector<int> base = hubui::filter_sort(items, "", q, spec);   // search applied, all consoles
    if (A.online_only) {
        std::vector<int> keep;
        for (int i : base) {
            if (i >= (int)A.lib.size()) {   // a friend's game: online-capable when the catalog says so
                const hubui::fg::Ghost& g = A.ghosts[i - A.lib.size()];
                const GameInfo* gc = g.cid.empty() ? nullptr : hub::find_game(g.cid);
                if (hubcaps::online_capable(hub::system_from_short(g.sys), gc)) keep.push_back(i);
                continue;
            }
            const hub::GameEntry& e = A.lib[i];
            if (entry_online(e)) keep.push_back(i);
        }
        base.swap(keep);
    }
    if (A.st.lib_favs_only) {   // Favorites chip: owned favorites only
        std::vector<int> keep;
        for (int i : base) if (i < (int)A.lib.size() && gstate(A.lib[i].key).favorite) keep.push_back(i);
        base.swap(keep);
    }
    A.genre_counts.clear();
    for (int i : base) ++A.genre_counts[genre_of_index(i)];
    if (!A.st.lib_genre.empty() && !A.lib.empty() && !A.scanning && !A.st.lib_favs_only && A.search_buf[0] == 0 && !A.online_only && !A.genre_counts.count(A.st.lib_genre)) { A.st.lib_genre.clear(); mark_dirty(); }   // the saved genre no longer exists in the library (only judged on an unfiltered, fully scanned library)
    if (!A.st.lib_genre.empty()) {
        std::vector<int> keep;
        for (int i : base) if ((genre_of_index(i)) == A.st.lib_genre) keep.push_back(i);
        base.swap(keep);
    }
    A.chip_counts.clear();
    A.chip_total = (int)base.size();
    A.vis.clear();
    for (int i : base) {
        ++A.chip_counts[items[i].system];
        if (A.sys_filter.empty() || items[i].system == A.sys_filter) A.vis.push_back(i);
    }
    A.secs = hubui::group_by_console(items, &A.vis, spec);
    if (A.st.lib_group_genre) {   // Group by Genre: A-Z genre sections, "Other" last; tiles keep their console label
        std::vector<int> idx = A.vis;
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return hubui::list_item_less(items[a], items[b], spec); });
        std::map<std::string, std::vector<int>> by;
        for (int i : idx) by[genre_of_index(i)].push_back(i);
        A.secs.clear();
        for (auto& kv : by) if (kv.first != "Other") A.secs.push_back({"other", kv.first, kv.second});
        if (by.count("Other")) A.secs.push_back({"other", "Other", by["Other"]});
    }

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
        g.online = entry_online(e);
        g.pinned = is_native(e);
        g.play_seconds = gs.play_seconds;
        g.added = added_unix(e.path);
        if (need_year) g.year = lib_year_of((int)A.sg.size());
        A.sg.push_back(g);
    }
    for (auto& gh : A.ghosts) {
        hubui::ShelfGame g;
        g.key = "ghost:" + gh.id;
        g.title = gh.title;
        g.system = gh.sys;
        g.ghost = true;
        if (need_year) g.year = lib_year_of((int)A.sg.size());
        A.sg.push_back(g);
    }
    A.shelves = hubui::compose_shelves(A.sg, 12, spec);
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
    ++A.vis_gen;
}

/* ---------- online capability ---------- */

bool supports_online(const hub::GameEntry& e) {
    /* hubcaps::online_capable: every game of N64/GB/GBC/GBA/3DS/GC (Dolphin NetPlay, any disc)/NDS (ds-link)/PS1 (rollback, multitap for 3-4)/PS2 (lockstep, multitap for 3-4).
     * Non-catalog GC/NDS/PS1 games are "untested" (online_untested). PS2 (retrorun + LRPS2, up to 4 players with the multitap) is in; Wii U = remote play.
     * A few games cannot be played in a room whatever the system (Buzz! quizzes: USB buzzers). Unit-tested against launch_online's branches. */
    const hub::GameInfo* cat = hub::find_game(e.catalog_id);
    return hubcaps::online_capable(e.system, cat) && hubcaps::online_game_block(e.system, cat, e.title).empty();
}
/* Why a game is greyed out in the room picker ("" when it can be played online). */
std::string online_block_reason(const hub::GameEntry& e) {
    const hub::GameInfo* cat = hub::find_game(e.catalog_id);
    if (!hubcaps::online_capable(e.system, cat)) return hubcaps::online_unsupported_reason(e.system);
    return hubcaps::online_game_block(e.system, cat, e.title);
}
/* Capable but not vetted (GC/DS/PS1 outside the catalog's online list): the picker tags it "Untested online". */
bool online_untested(const hub::GameEntry& e) { return hubcaps::online_untested(e.system, hub::find_game(e.catalog_id)); }
/* Online player cap for a room (online_max_for; 0 online_max = max_players). Every online limit uses this. */
int max_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return hub::online_max_for(*g);
    return hubcaps::default_online_max(e.system);   // not in the catalog: what the system's online path carries for an unvetted game (online_caps.h)
}
/* Why that cap ("" when it is simply the console's four): the room card and the "too many players" reason carry it. */
std::string player_limit_note(const hub::GameEntry& e) {
    return hubcaps::online_limit_note(e.system, hub::find_game(e.catalog_id), max_players_for(e));
}
std::string player_limit_why(const hub::GameEntry& e) {
    return hubcaps::online_limit_note(e.system, hub::find_game(e.catalog_id), max_players_for(e), true);
}
/* Local player count shown on cards (not the online cap). */
int local_players_for(const hub::GameEntry& e) {
    if (const GameInfo* g = hub::find_game(e.catalog_id)) return g->max_players;
    return hubcaps::default_local_max(e.system);   // not in the catalog: the old room-cap values (GB/GBC/DS 2, GBA/3DS 4), raised for PS1/Wii U (4); PSP 1
}
/* How a game is named in lobby messages: the N64 catalog id (back-compat) or the library key. */
std::string room_id(const hub::GameEntry& e) { return e.catalog_id.empty() ? e.key : e.catalog_id; }
/* Remote play: a guest needs no copy of the game, so a catalog remote-play id resolves to a stand-in entry (title, system, catalog id;
 * no path) when the library has none. Nothing launches from it locally; launch_online streams instead. */
static const hub::GameEntry* remote_stand_in(const std::string& id) {
    static hub::GameEntry slot;
    const GameInfo* g = hub::find_game(id);
    if (!g || !g->online) return nullptr;
    hub::System s = hub::system_from_short(g->system.c_str());
    if (!hubcaps::remote_play_game(s, g)) return nullptr;
    slot = hub::GameEntry();
    slot.key = "remote:" + id;
    slot.system = s;
    slot.title = g->title;
    slot.catalog_id = id;
    return &slot;
}
bool is_remote_stand_in(const hub::GameEntry& e) { return e.key.compare(0, 7, "remote:") == 0; }
bool remote_play_entry(const hub::GameEntry& e) { return hubcaps::remote_play_game(e.system, hub::find_game(e.catalog_id)); }
const hub::GameEntry* find_room_entry(const std::string& id) {
    for (auto& e : A.lib)
        if (supports_online(e) && room_id(e) == id) return &e;
    return remote_stand_in(id);
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
        /* Same catalog game, different region/revision: send the content-based key as a version id (see hubcaps::add_version_id). Older RetroHubs publish none and are never blocked by it. */
        hubcaps::add_version_id(games, e.system, e.catalog_id, e.key);
        if (keys) {
            keys->push_back(e.key);
            /* The room names a catalog game by its catalog id (room_id), so advertise that too, or game_selectable never finds non-N64 catalog games (GameCube) */
            if (!e.catalog_id.empty() && e.key.compare(0, 4, "n64:") != 0 && std::find(keys->begin(), keys->end(), e.catalog_id) == keys->end()) keys->push_back(e.catalog_id);
        }
    }
}

