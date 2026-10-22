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

enum class System { Unknown = 0, N64, GBA, GB, GBC, GC,
                    N3DS, WiiU };   // appended: 3DS (Azahar), Wii U (Cemu)

const char* system_name(System s);   // "Nintendo 64", "Game Boy Advance", "Game Boy", "Game Boy Color", "GameCube"
const char* system_short(System s);  // "n64", "gba", "gb", "gbc", "gc" "3ds" "wiiu"  (stable; used in keys and paths)
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
    std::string issue;        // "" ok | "extract" zipped 3DS image Azahar cannot open | "encrypted" 3DS image needing AES keys |
                              // "keys" Wii U .wud/.wux needing keys.txt
};

/* Identify one file by extension + header. false if not a recognized game. Cheap:
 * reads headers only, except N64 (full MD5) and GBA (CRC32) which hash the file. */
bool identify_game(const std::string& path, GameEntry* out);

/* Recursively scan folders. Extensions: .z64 .v64 .n64 .gba .gb .gbc .iso .gcm .rvz .wia .ciso .gcz
 * Callback gets (files_done, files_total) for a progress bar; may be null. It is always
 * invoked on the calling thread. Results are cached in data_dir()/library_cache.json
 * (path + size + mtime), so unchanged files are not re-identified. */
std::vector<GameEntry> scan_library(const std::vector<std::string>& folders,
                                    const std::function<void(int, int)>& progress);

/* Zipped 3DS images (GameEntry::issue == "extract"): Azahar cannot read .zip, so the player may unpack one on request
 * (never done silently). Target: data_dir()/rom-cache/3ds/<key>/<image name>. */
std::string extracted_3ds_path(const GameEntry& e);   // the unpacked image if it exists, else ""
/* progress(done, total) returns false to cancel. On success *out_path is the image. Checks free disk space first. */
bool extract_3ds_zip(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress,
                     std::string* out_path, std::string* err);
std::string extract_3ds_dir(const GameEntry& e);      // where it would go (for the confirm dialog)
uint64_t zip_3ds_unpacked_size(const GameEntry& e);   // size of the image inside the zip (central directory only), 0 if unknown

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

/* ---- additions (tests / diagnostics) ---- */

/* Number of times a file was actually identified (header read / hashed) since process start.
 * Cache hits in scan_library do not count. */
uint64_t library_identify_count();

/* Redirect library.json / library_cache.json to another directory (trailing slash optional);
 * "" restores data_dir(). Flushes pending state, then drops the in-memory state so it is
 * reloaded from the new location. */
void library_set_data_dir(const std::string& dir);

} // namespace hub
