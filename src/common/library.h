#pragma once
/* Multi-system game library: identification of any ROM/disc image by header,
 * plus per-user library state (favorites, recently played, play time).
 * Replaces the N64-only catalog lookup for discovery; games.h's catalog() stays
 * as curated metadata (e.g. which N64 games have netplay presets). */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hub {

enum class System { Unknown = 0, N64, GBA, GB, GBC, GC };

const char* system_name(System s);   // "Nintendo 64", "Game Boy Advance", "Game Boy", "Game Boy Color", "GameCube"
const char* system_short(System s);  // "n64", "gba", "gb", "gbc", "gc"  (stable; used in keys and paths)
System system_from_short(const std::string& s);

struct GameEntry {
    /* Stable identity across PCs, used for saves and library state:
     *   n64:<MD5 of .z64-order image>     gba:<4-char game code>-<CRC32 of the whole file, hex>
     *   gb/gbc:<header title>-<global checksum hex>   gc:<6-char disc ID>-<disc number>
     * Lowercase, filesystem-safe ([a-z0-9._-]). */
    std::string key;
    System system = System::Unknown;
    std::string title;        // header title, prettified; falls back to file name
    std::string serial;       // N64 game ID (0x3B), GBA game code (0xAC), GC disc ID; "" if none
    std::string path;         // absolute UTF-8 path
    uint64_t size = 0;
    std::string catalog_id;   // games.h catalog id when known (e.g. "mp1"), else ""
};

/* Identify one file by extension + header. false if not a recognized game. Cheap:
 * reads headers only, except N64 (full MD5) and GBA (CRC32) which hash the file. */
bool identify_game(const std::string& path, GameEntry* out);

/* Recursively scan folders. Extensions: .z64 .v64 .n64 .gba .gb .gbc .iso .gcm .rvz .ciso .gcz
 * Callback gets (files_done, files_total) for a progress bar; may be null. */
std::vector<GameEntry> scan_library(const std::vector<std::string>& folders,
                                    const std::function<void(int, int)>& progress);

/* Per-user library state, persisted to data_dir()/library.json. */
struct GameState {
    bool favorite = false;
    uint64_t last_played = 0;   // unix seconds
    uint64_t play_seconds = 0;
};
GameState game_state(const std::string& key);
void set_favorite(const std::string& key, bool fav);
void record_play(const std::string& key, uint64_t started_unix, uint64_t seconds);
void flush_library_state();

} // namespace hub
