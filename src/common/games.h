#pragma once
/* Game catalog + ROM identification + paths shared by RetroHub and n64run. */

#include <cstdint>
#include <string>
#include <vector>

namespace hub {

enum class Engine { Native, Mupen64Plus };

struct GameInfo {
    std::string id;          // "mp1"
    std::string title;       // "Mario Party"
    Engine engine;
    int max_players;         // 4
    std::vector<std::string> rom_md5s;  // uppercase hex MD5 of the .z64-order image; accepted dumps
    std::string native_exe;  // Native only: path relative to the hub dir, e.g. "games/smashClassic/BattleShip.exe"
};

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
