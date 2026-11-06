#pragma once
/* Transfer Pak (Plan 4 Wave 3, X1): pure logic. A Game Boy / Game Boy Color cartridge from the player's library plugs into
 * controller 1 of an N64 game (offline only). No ImGui, no Windows, no file system calls except where a caller passes data in.
 *
 *   - which N64 games use the pak, and which GB/GBC games fit each of them (best match first)
 *   - the stored choice per N64 game (JSON, hub::data_dir()) and the default (exactly one fit -> use it, otherwise none)
 *   - the GB save file: the exact vault battery file retrorun uses ("<stem>.srm", docs/SAVES.md, save_stem_arg in launch.cpp)
 *   - the expected save size for a cartridge header (the core reads the file at exactly that size)
 *   - backups (last 3) and the command-line flags for n64run
 */
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace hubui {
namespace tpak {

constexpr int kKeepBackups = 3;

inline std::string norm(const std::string& s) {   // lowercase letters and digits only
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

/* Which kind of games a Transfer Pak game accepts. */
enum class Kind { None, Pokemon1, Pokemon2, MarioGolf, MarioTennis, PerfectDark };

inline Kind kind_for(const std::string& n64_catalog_id) {
    if (n64_catalog_id == "pokestadium") return Kind::Pokemon1;
    if (n64_catalog_id == "pokestadium2") return Kind::Pokemon2;
    if (n64_catalog_id == "mariogolf") return Kind::MarioGolf;
    if (n64_catalog_id == "mariotennis") return Kind::MarioTennis;
    if (n64_catalog_id == "perfectdark") return Kind::PerfectDark;
    return Kind::None;
}
inline bool uses_transfer_pak(const std::string& n64_catalog_id) { return kind_for(n64_catalog_id) != Kind::None; }

/* 1 = first generation (Red, Blue, Yellow), 2 = second (Gold, Silver, Crystal), 0 = not a Pokemon game of those. Catalog id first, then the title
 * ("POKEMON RED", "POKEMON YEL", "POKEMON_GLD", "PM_CRYSTAL", "Pokemon - Blue Version (USA)"). Spin-offs (Pinball, Puzzle, Trading Card ...) are 0. */
inline int pokemon_gen(const std::string& catalog_id, const std::string& title) {
    if (catalog_id == "pokered" || catalog_id == "pokeblue" || catalog_id == "pokeyellow") return 1;
    if (catalog_id == "pokegold" || catalog_id == "pokesilver" || catalog_id == "pokecrystal") return 2;
    if (!catalog_id.empty()) return 0;   // another catalog game
    const std::string t = norm(title);
    const bool poke = t.rfind("pokemon", 0) == 0 || t.rfind("pm", 0) == 0;
    if (!poke) return 0;
    for (const char* bad : {"pinball", "puzzle", "trading", "card", "tcg", "stadium", "snap", "picross", "dash", "tcg2"})
        if (t.find(bad) != std::string::npos) return 0;
    for (const char* g : {"red", "blue", "yel"}) if (t.find(g) != std::string::npos) return 1;
    for (const char* g : {"gold", "gld", "silver", "slv", "crystal"}) if (t.find(g) != std::string::npos) return 2;
    return 0;
}

struct GbGame {
    std::string key, title, catalog_id;
    bool color = false;   // GBC (a Game Boy Color only cartridge); Mario Golf / Tennis / Perfect Dark are GBC games
};

/* 0 = does not fit; higher = better match. */
inline int match_rank(Kind k, const GbGame& g) {
    const std::string t = norm(g.title);
    switch (k) {
        case Kind::Pokemon1: { int n = pokemon_gen(g.catalog_id, g.title); return n == 1 ? 100 : 0; }   // Stadium reads Red, Blue and Yellow only
        case Kind::Pokemon2: { int n = pokemon_gen(g.catalog_id, g.title); return n == 2 ? 100 : n == 1 ? 80 : 0; }   // Stadium 2: Gold / Silver / Crystal first, and the first generation too
        case Kind::MarioGolf: return t.find("mariogolf") != std::string::npos ? 100 : 0;
        case Kind::MarioTennis: return t.find("mariotennis") != std::string::npos ? 100 : 0;
        case Kind::PerfectDark: return t == "perfectdark" || t.rfind("perfectdark", 0) == 0 ? 100 : 0;
        case Kind::None: break;
    }
    return 0;
}

/* The player's GB / GBC games that fit this N64 game, best match first (then by title, then key so the order is stable). */
inline std::vector<GbGame> compatible(const std::string& n64_catalog_id, const std::vector<GbGame>& library) {
    std::vector<GbGame> out;
    const Kind k = kind_for(n64_catalog_id);
    if (k == Kind::None) return out;
    std::vector<std::pair<int, GbGame>> v;
    for (const GbGame& g : library) { int r = match_rank(k, g); if (r > 0) v.push_back({r, g}); }
    std::stable_sort(v.begin(), v.end(), [](const std::pair<int, GbGame>& a, const std::pair<int, GbGame>& b) {
        if (a.first != b.first) return a.first > b.first;
        const std::string x = norm(a.second.title), y = norm(b.second.title);
        if (x != y) return x < y;
        return a.second.key < b.second.key;
    });
    for (auto& p : v) out.push_back(p.second);
    return out;
}

/* The cartridge to use: stored = nullptr (never chosen) | "" (the player chose None) | a GB game key.
 * Never chosen: exactly one fit -> it, otherwise none. A stored key that is no longer a fit (game removed) falls back to the default. */
inline std::string resolve_choice(const std::string* stored, const std::vector<GbGame>& fits) {
    if (stored && stored->empty()) return std::string();
    if (stored) for (const GbGame& g : fits) if (g.key == *stored) return g.key;
    return fits.size() == 1 ? fits[0].key : std::string();
}

/* ---- the GB save: the vault battery file retrorun uses ---- */

inline std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

inline std::string path_stem(const std::string& path) {   // file name without directory and last extension (both slash kinds)
    size_t sl = path.find_last_of("\\/");
    std::string f = sl == std::string::npos ? path : path.substr(sl + 1);
    size_t dot = f.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? f : f.substr(0, dot);
}

/* The save file NAME (not path) retrorun uses for this game: "<save_stem or rom stem>.srm"; when that file is missing and the folder holds
 * exactly one .srm, that one (retrorun's rename-tolerance rule). `existing` = file names in the battery folder. */
inline std::string save_file_name(const std::string& rom_path, const std::string& save_stem, const std::vector<std::string>& existing) {
    const std::string stem = save_stem.empty() ? path_stem(rom_path) : save_stem;
    const std::string want = stem + ".srm";
    std::string only;
    int srms = 0;
    for (const std::string& n : existing) {
        if (lower(n) == lower(want)) return n;
        if (n.size() > 4 && lower(n.substr(n.size() - 4)) == ".srm") { only = n; srms++; }
    }
    return srms == 1 ? only : want;
}

/* Save size the core expects for a cartridge (header bytes 0x147 type, 0x149 RAM code); 0 = no save RAM or unreadable header.
 * Matches mupen64plus-core device/gb/gb_cart.c. */
inline size_t expected_ram_size(const std::vector<uint8_t>& header /* at least 0x14A bytes */) {
    if (header.size() < 0x14A) return 0;
    const uint8_t type = header[0x147];
    switch (header[0x149]) {
        case 0x00: return (type == 0x05 || type == 0x06) ? 512 : 0;   // MBC2 has 512 half-bytes built in
        case 0x01: return 1 * 0x800;
        case 0x02: return 4 * 0x800;
        case 0x03: return 16 * 0x800;
        case 0x04: return 64 * 0x800;
        case 0x05: return 32 * 0x800;
    }
    return 0;
}

/* The core reads exactly the expected size (a shorter file leaves garbage, a longer one is cut when the core writes it back): only a save
 * of the right size, or none yet, is safe to plug in. */
inline bool save_size_ok(size_t expected, bool exists, size_t actual) { return expected == 0 || !exists || actual == expected; }

/* ---- backups: <data_dir>\tpak_backups\<gb key>\<save name>.<stamp>.bak; keep the newest kKeepBackups ---- */

inline std::string backup_name(const std::string& save_name, const std::string& stamp) { return save_name + "." + stamp + ".bak"; }

/* Names to delete so only the newest `keep` remain. Stamps sort as text (yyyymmddhhmmss). Only names ending in .bak count. */
inline std::vector<std::string> backups_to_prune(std::vector<std::string> names, int keep = kKeepBackups) {
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return n.size() < 4 || lower(n.substr(n.size() - 4)) != ".bak"; }), names.end());
    std::sort(names.begin(), names.end());
    std::vector<std::string> del;
    if ((int)names.size() > keep) del.assign(names.begin(), names.end() - keep);
    return del;
}

/* ---- n64run flags: " --tpak-rom <gb path> --tpak-save <gb save path>" (each starting with a space), "" for nothing ---- */

template <class Quote>
std::string launch_args(const std::string& gb_rom, const std::string& gb_save, Quote quote) {
    if (gb_rom.empty()) return std::string();
    std::string a = " --tpak-rom " + quote(gb_rom);
    if (!gb_save.empty()) a += " --tpak-save " + quote(gb_save);
    return a;
}

/* Online (or netplay) runs never carry the flags: same rule n64run applies. */
inline bool args_allowed(const std::string& other_args) { return other_args.find("--net") == std::string::npos; }

}  // namespace tpak
}  // namespace hubui
