#pragma once
/* Plan 4 Wave 3 X2: GameCube <-> GBA through Dolphin's integrated GBA (mGBA inside Dolphin, SI device "GBA (Integrated)").
 * Pure logic (no ImGui, no App): which GameCube games support it, which GBA games fit, the choice store, Dolphin.ini text and the save round trip.
 * Test: test_gba_link.cpp.
 *
 * Dolphin facts (source: dolphin-emu/dolphin master; Core/HW/SI/SI_Device.h, Core/Config/MainSettings.cpp, Core/HW/GBACore.cpp):
 *  - SIDevice values: 6 = standard pad, 12 = Wii U adapter, 13 = SIDEVICE_GC_GBA_EMULATED ("GBA (Integrated)"). Only 13 is used here.
 *  - Dolphin.ini [GBA]: BIOS = <gba_bios.bin>, Rom1..Rom4 = <rom per SI port>, SavesPath = <folder>, SavesInRomPath = False.
 *    (a port with a BIOS but an empty Rom is a GBA the game boots by joybus multiboot: Four Swords, Tingle Tuner, Crystal Chronicles.)
 *  - The save of the GBA on SI port N (0-based) is <SavesPath>/<rom file stem>-<N+1>.sav : raw mGBA save, the same bytes retrorun's mGBA
 *    core keeps in <battery>/<rom stem>.srm. mGBA appends a 16 byte RTC block to a .sav of a cartridge with a clock (Ruby / Sapphire / Emerald);
 *    the vault's .srm never has it, so it is cut off when the save goes back.
 *  - Port 1 (index 0) is never touched: it keeps the player's own pad. */
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "json.h"
#include "logic.h"

namespace hubgba {

constexpr int kSiGbaEmulated = 13;
constexpr const char* kControllerChoice = "ctl";   // GBA as a controller: no ROM, the game sends its own program over the cable
constexpr const char* kGameChoicePrefix = "gba:";  // gba:<library key of the GBA game>

enum class Need { Controller, Rom };

struct Target {
    const char* id3;        // first three characters of the GameCube disc ID (the 4th is the region)
    const char* title;
    Need need;
    int ports;              // GBAs the game can use besides the pad on port 1 (UI shows ports 2..1+ports)
    const char* partners;   // comma list: first three characters of the GBA game codes that fit ("" = none needed)
    const char* help;       // one friendly sentence
};

inline const std::vector<Target>& targets() {
    static const std::vector<Target> t = {
        {"GC6", "Pokemon Colosseum", Need::Rom, 1, "AXV,AXP,BPR,BPG,BPE", "Trade Pokemon with Ruby, Sapphire, FireRed, LeafGreen or Emerald."},
        {"GXX", "Pokemon XD", Need::Rom, 1, "AXV,AXP,BPR,BPG,BPE", "Trade Pokemon with Ruby, Sapphire, FireRed, LeafGreen or Emerald."},
        {"GPX", "Pokemon Box", Need::Rom, 1, "AXV,AXP,BPR,BPG,BPE", "Move Pokemon from Ruby, Sapphire, FireRed, LeafGreen or Emerald."},
        {"G4S", "Zelda: Four Swords Adventures", Need::Controller, 3, "", "Use a GBA as a controller: it shows your own screen. No GBA game needed."},
        {"GCC", "Final Fantasy Crystal Chronicles", Need::Controller, 3, "", "Use a GBA as a controller for your character. No GBA game needed."},
        {"GZL", "The Legend of Zelda: The Wind Waker", Need::Controller, 1, "", "Use a GBA as the Tingle Tuner. No GBA game needed."},
        {"GM8", "Metroid Prime", Need::Rom, 1, "AMT", "Link with Metroid Fusion to unlock the Fusion Suit."},
    };
    return t;
}

inline std::string upper3(const std::string& s) {
    std::string r = s.substr(0, 3);
    for (char& c : r) c = (char)std::toupper((unsigned char)c);
    return r;
}

/* The target for a GameCube disc ID ("GM8E01"), or null when the game has no GBA link. */
inline const Target* target_for(const std::string& gc_serial) {
    if (gc_serial.size() < 4) return nullptr;
    const std::string k = upper3(gc_serial);
    for (const Target& t : targets()) if (k == t.id3) return &t;
    return nullptr;
}

/* True when this GBA game code ("AXVE") is one the target's text names. */
inline bool partner_fits(const Target& t, const std::string& gba_serial) {
    if (gba_serial.size() < 4 || !*t.partners) return false;
    const std::string k = upper3(gba_serial);
    std::string p = t.partners;
    size_t pos = 0;
    while (pos < p.size()) {
        size_t c = p.find(',', pos);
        if (c == std::string::npos) c = p.size();
        if (p.substr(pos, c - pos) == k) return true;
        pos = c + 1;
    }
    return false;
}

struct Cand { std::string key, title, serial, path; };

inline std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

/* Indices into `all`, best matches first (by title), then every other game (by title). *n_match = how many are matches. */
inline std::vector<size_t> ordered(const Target* t, const std::vector<Cand>& all, size_t* n_match = nullptr) {
    std::vector<size_t> hit, rest;
    for (size_t i = 0; i < all.size(); ++i) (t && partner_fits(*t, all[i].serial) ? hit : rest).push_back(i);
    auto by_title = [&](size_t a, size_t b) { return lower(all[a].title) < lower(all[b].title); };
    std::stable_sort(hit.begin(), hit.end(), by_title);
    std::stable_sort(rest.begin(), rest.end(), by_title);
    if (n_match) *n_match = hit.size();
    hit.insert(hit.end(), rest.begin(), rest.end());
    return hit;
}

/* ---- the choice per port (index 0 = port 1 is never used) ---- */
using Ports = std::array<std::string, 4>;   // "" = Off | "ctl" | "gba:<key>"

enum class Kind { Off, Controller, Game };
struct Choice { Kind kind = Kind::Off; std::string key; };
inline Choice parse_choice(const std::string& s) {
    Choice c;
    if (s == kControllerChoice) c.kind = Kind::Controller;
    else if (s.rfind(kGameChoicePrefix, 0) == 0 && s.size() > 4) { c.kind = Kind::Game; c.key = s.substr(4); }
    return c;
}
inline std::string game_choice(const std::string& key) { return std::string(kGameChoicePrefix) + key; }
inline bool any_on(const Ports& p) { for (int i = 1; i < 4; ++i) if (parse_choice(p[i]).kind != Kind::Off) return true; return false; }

/* gba_link.json: {"games":{"<gc key>":{"ports":["","ctl","gba:gba:axve-1234abcd",""]}}}  (all Off = the key is left out) */
inline std::string store_to_json(const std::map<std::string, Ports>& m) {
    json::Value root = json::make_object(), games = json::make_object();
    for (const auto& kv : m) {
        if (!any_on(kv.second)) continue;
        json::Value g = json::make_object(), arr = json::make_array();
        for (const std::string& s : kv.second) arr.a.push_back(json::make_string(s));
        g.o.push_back({"ports", arr});
        games.o.push_back({kv.first, g});
    }
    root.o.push_back({"games", games});
    return json::stringify(root);
}
inline bool store_from_json(const std::string& text, std::map<std::string, Ports>* out) {
    out->clear();
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    const json::Value* games = root.get("games");
    if (!games || !games->is_object()) return true;
    for (const auto& kv : games->o) {
        const json::Value* ports = kv.second.get("ports");
        if (!ports || !ports->is_array()) continue;
        Ports p;
        for (size_t i = 1; i < ports->a.size() && i < 4; ++i) if (ports->a[i].is_string()) p[i] = ports->a[i].s;
        if (any_on(p)) (*out)[kv.first] = p;
    }
    return true;
}

/* ---- Dolphin.ini ---- */
struct IniPlan {
    std::array<bool, 4> on{};              // SIDevice = GBA (Integrated) on this port
    std::array<std::string, 4> rom{};      // ROM of this port ("" = the game boots the GBA itself)
    std::string bios;                      // the player's gba_bios.bin
    std::string saves_dir;                 // Dolphin's GBA save folder for this launch
};
inline std::string slashes(std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return s; }

/* Dolphin.ini text with the GBA ports set. Keys of ports that stay a pad are not touched (prepare_dolphin sets those every launch). */
inline std::string ini_apply(const std::string& in, const IniPlan& p) {
    std::string t = in;
    std::string sd = slashes(p.saves_dir);
    if (!sd.empty() && sd.back() != '/') sd += '/';   // Dolphin adds the file name straight on
    t = hubui::ini_set(t, "GBA", "BIOS", slashes(p.bios));
    t = hubui::ini_set(t, "GBA", "SavesInRomPath", "False");
    t = hubui::ini_set(t, "GBA", "SavesPath", sd);
    for (int i = 1; i < 4; ++i) {
        if (!p.on[i]) continue;
        t = hubui::ini_set(t, "GBA", "Rom" + std::to_string(i + 1), slashes(p.rom[i]));
        t = hubui::ini_set(t, "Core", "SIDevice" + std::to_string(i), std::to_string(kSiGbaEmulated));
    }
    return t;
}

/* ---- saves ---- */
namespace fs = std::filesystem;
inline fs::path U(const std::string& s) { return fs::u8path(s); }

inline std::string file_stem_of(const std::string& path) {
    size_t sl = path.find_last_of("/\\");
    std::string f = sl == std::string::npos ? path : path.substr(sl + 1);
    size_t dot = f.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? f : f.substr(0, dot);
}
/* retrorun (mGBA core): <battery>\<stem>.srm, stem = GameEntry::save_stem or the ROM file's stem (save_stem_arg in launch.cpp). */
inline std::string vault_file(const std::string& battery_dir, const std::string& rom_path, const std::string& save_stem) {
    std::string d = battery_dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d + "\\" + (save_stem.empty() ? file_stem_of(rom_path) : save_stem) + ".srm";
}
/* Dolphin (Core::GetSavePath): <SavesPath>/<rom file stem>-<SI port + 1>.sav */
inline std::string dolphin_save_file(const std::string& saves_dir, const std::string& rom_path, int si_port) {
    std::string d = saves_dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d + "\\" + file_stem_of(rom_path) + "-" + std::to_string(si_port + 1) + ".sav";
}

inline bool read_all(const std::string& p, std::string* out) {
    std::ifstream f(U(p), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
/* Write through a temp file + rename, so a crash never leaves a half save. */
inline bool write_atomic(const std::string& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(U(p).parent_path(), ec);
    const std::string tmp = p + ".hubtmp";
    {
        std::ofstream f(U(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    fs::rename(U(tmp), U(p), ec);
    if (ec) { fs::remove(U(tmp), ec); return false; }
    return true;
}
/* A copy of `file` in `dir` that never overwrites an earlier backup: <name>.<n>.bak */
inline bool backup_file(const std::string& file, const std::string& dir) {
    std::string data;
    if (!read_all(file, &data)) return true;   // nothing to keep
    std::error_code ec;
    fs::create_directories(U(dir), ec);
    const std::string name = U(file).filename().u8string();
    for (int n = 1; n < 10000; ++n) {
        std::string b = dir + "\\" + name + "." + std::to_string(n) + ".bak";
        if (!fs::exists(U(b), ec)) return write_atomic(b, data);
    }
    return false;
}

/* mGBA's .sav carries a 16 byte clock block after the save for a cartridge with an RTC; the vault copy is the bare save. */
inline std::string normalize_save(const std::string& b) {
    static const size_t canon[] = {512, 8192, 32768, 65536, 131072};
    auto is_c = [&](size_t n) { return std::find(std::begin(canon), std::end(canon), n) != std::end(canon); };
    if (!is_c(b.size()) && b.size() > 16 && is_c(b.size() - 16)) return b.substr(0, b.size() - 16);
    return b;
}
inline bool all_blank(const std::string& b) { for (char c : b) if ((unsigned char)c != 0xFF) return false; return true; }

struct Staged {
    int port = 0;                  // SI port index (1..3)
    std::string game_key, system_short, title;   // the GBA game (vault key)
    std::string vault, dolphin, backup_dir;
};

/* Before launch: the vault save becomes Dolphin's save for this port. A file Dolphin already has under that name is backed up first
 * (it may be an unsynced result of an earlier session), then replaced; without a vault save Dolphin starts from an empty one. */
inline bool stage_save(const Staged& s, std::string* err) {
    std::error_code ec;
    fs::create_directories(U(s.dolphin).parent_path(), ec);
    std::string vault, existing;
    std::error_code ec0;
    const bool have_vault = read_all(s.vault, &vault);
    if (!have_vault && fs::exists(U(s.vault), ec0)) { if (err) *err = "the vault save could not be read"; return false; }   // locked for a moment: try next time
    if (read_all(s.dolphin, &existing)) {
        if (!(have_vault && existing == vault) && !backup_file(s.dolphin, s.backup_dir)) { if (err) *err = "could not back up the old GBA save"; return false; }
        fs::remove(U(s.dolphin), ec);
    }
    if (have_vault && !write_atomic(s.dolphin, vault)) { if (err) *err = "could not copy the GBA save"; return false; }
    return true;
}

enum class Back { Nothing, Copied, Failed };

/* After Dolphin ended: a changed save goes back to the vault under retrorun's name. The vault's old file is backed up first. */
inline Back finish_save(const Staged& s, std::string* err) {
    std::string raw;
    if (!read_all(s.dolphin, &raw) || raw.empty() || raw.size() > (1u << 20)) return Back::Nothing;
    const std::string now = normalize_save(raw);
    std::string vault;
    const bool have_vault = read_all(s.vault, &vault);
    std::error_code ec0;
    if (!have_vault && fs::exists(U(s.vault), ec0)) { if (err) *err = "the vault save could not be read"; return Back::Failed; }   // never overwrite what cannot be read
    if (have_vault ? now == vault : all_blank(now)) return Back::Nothing;
    if (have_vault && !backup_file(s.vault, s.backup_dir)) { if (err) *err = "could not back up the vault save"; return Back::Failed; }
    if (!write_atomic(s.vault, now)) { if (err) *err = "could not write the vault save"; return Back::Failed; }
    return Back::Copied;
}

}  // namespace hubgba
