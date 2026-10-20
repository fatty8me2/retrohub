#pragma once
/* Game catalog + ROM identification + paths shared by RetroHub and n64run. */

#include <cstdint>
#include <string>
#include <vector>

namespace hub {

enum class Engine { Native, Mupen64Plus, Libretro, Dolphin };

/* Curated metadata for well-known games. Any ROM plays without an entry here;
 * an entry adds a nicer title, player count, online flag and a blurb, and
 * lets the hub show "top games" the player doesn't own yet. */
struct GameInfo {
    std::string id;          // "mp1"
    std::string title;       // "Mario Party"
    Engine engine;
    int max_players;         // 4
    std::vector<std::string> rom_md5s;  // uppercase hex MD5 of the .z64-order image; accepted dumps
    std::string native_exe;  // Native only: path relative to the hub dir, e.g. "games/smashClassic/BattleShip.exe"
    // ---- added for the multi-system catalog (defaults keep older entries valid) ----
    std::string system = "n64";         // library.h system_short(): n64 gba gb gbc gc
    std::vector<std::string> serials;   // header IDs, matched case-insensitively against GameEntry::serial:
                                        //   N64 4-char game ID at 0x3B (e.g. "NSME"), GBA 4-char code (e.g. "BPEE"),
                                        //   GC 6-char disc ID (e.g. "GALE01"). Region variants allowed.
    bool online = false;     // fun with friends online (2+ players, works with lockstep netplay)
    int year = 0;
    std::string blurb;       // one line shown on the card / details panel
    std::string nointro;     // No-Intro name of the primary (USA) release, e.g.
                             // "Mario Kart 64 (USA)" -- used to fetch cover art
    std::string genre;       // one of genres(): "Party" "Fighting" ... "" = unknown (hub files it under "Other")
};

/* Allowed GameInfo::genre values, in the order the hub shows genre shelves. */
const std::vector<std::string>& genres();

/* Catalog lookup for a scanned game: by catalog_id/MD5 first, then serial + system. */
const GameInfo* find_game_for(const std::string& system_short, const std::string& serial, const std::string& md5);

const std::vector<GameInfo>& catalog();
const GameInfo* find_game(const std::string& id);

struct RomFile {
    std::string path;
    std::string md5;         // of the normalized (.z64 byte order) image, uppercase hex
    std::string game_id;     // "" if unknown
    std::string header_name; // internal name at 0x20 (trimmed)
};

/* Read a ROM, normalize .v64/.n64 byte order to .z64, return false if not an N64 ROM. */
bool load_rom(const std::string& path, std::vector<uint8_t>* z64_image);
/* Identify one file (MD5 + catalog lookup). */
bool identify_rom(const std::string& path, RomFile* out);
/* Recursively scan a folder for .z64/.v64/.n64 files. */
std::vector<RomFile> scan_roms(const std::string& folder);

std::string md5_hex(const uint8_t* data, size_t len);  // uppercase

/* Directory of the running exe, with trailing slash. */
std::string exe_dir();
/* Per-user data dir (portable: <exe_dir>/userdata/), created on demand. */
std::string data_dir();

} // namespace hub
