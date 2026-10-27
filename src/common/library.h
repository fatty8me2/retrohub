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
                    N3DS, WiiU,   // appended: 3DS (Azahar), Wii U (Cemu)
    NDS,
    PS2,     // PlayStation 2 (PCSX2, downloaded on demand)
    PS1,     // PlayStation (Beetle PSX via retrorun)
    NES,     // Nintendo Entertainment System (libretro via retrorun)
    SNES };  // Super Nintendo (libretro via retrorun)
/* GB/GBC/GBA run as one lockstepped console online (retrorun --net); the room can share the host's save with everyone. */
inline bool is_lockstep_gb_system(System s) { return s == System::GB || s == System::GBC || s == System::GBA; }


const char* system_name(System s);   // "Nintendo 64", "Game Boy Advance", "Game Boy", "Game Boy Color", "GameCube"
const char* system_short(System s);  // "n64", "gba", "gb", "gbc", "gc" "3ds" "wiiu" "nes" "snes"  (stable; used in keys and paths)
System system_from_short(const std::string& s);

/* NES / SNES cartridge header detection (pure; unit-tested). */
struct NesHeader {
    bool nes2 = false, battery = false, trainer = false;
    unsigned mapper = 0;
    uint64_t prg = 0, chr = 0;     // bytes
    uint64_t data_off = 16;        // first byte of PRG (after the 16-byte header and an optional 512-byte trainer)
};
/* iNES / NES 2.0 ("NES"). false if the magic is missing or the declared PRG+CHR does not fit file_size. */
bool nes_parse_header(const uint8_t* h, size_t n, uint64_t file_size, NesHeader* out);
/* SNES dumps may carry a 512-byte copier header (size % 1024 == 512). Returns 512 or 0. */
size_t snes_copier_header_size(uint64_t file_size);
struct SnesInfo { bool valid = false; bool hirom = false; bool checksum_ok = false; size_t header = 0; std::string title; };
/* Looks for the internal header at LoROM 0x7FC0 / HiROM 0xFFC0 (after any copier header). valid=false if neither looks real. */
SnesInfo snes_probe(const uint8_t* d, size_t n);

struct GameEntry {
    /* Stable identity across PCs, used for saves and library state:
     *   n64:<MD5 of .z64-order image>     gba:<4-char game code>-<CRC32 of the whole file, hex>
     *   gb/gbc:<header title>-<global checksum hex>   gc:<6-char disc ID>-<disc number>
     *   nds:<4-char game code>-<ROM version hex><header CRC16 hex>
     *   ps1:<boot serial, e.g. slus-00594>   (homebrew: ps1:file-<name>)
     * Lowercase, filesystem-safe ([a-z0-9._-]). */
    std::string key;
    System system = System::Unknown;
    std::string title;        // header title, prettified; falls back to file name
    std::string serial;       // N64 game ID (0x3B), GBA game code (0xAC), GC disc ID; "" if none
    std::string path;         // absolute UTF-8 path (for a zipped ROM: the extracted copy under rom-cache\)
    std::string save_stem;    // "" = stem of path. Set for a zipped ROM: the inner entry's stem (DS saves are <stem>.srm)
    uint64_t size = 0;
    std::string catalog_id;   // games.h catalog id when known (e.g. "mp1"), else ""
    uint64_t unpacked = 0;    // archived GC disc: size of the image inside (from the scan; 0 = unknown)
    int gc_rev = -1;          // GC disc version byte (header 0x07); -1 = not read. Filled on demand (cheats), not cached.
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

/* Two files that identify as the same game (same key): true when the challenger should be listed
 * instead of the holder. An unzipped image beats a .zip/.7z copy; otherwise the holder (first found) stays. */
bool dedupe_prefers(const std::string& challenger_path, const std::string& holder_path);

/* Zipped 3DS images (GameEntry::issue == "extract"): Azahar cannot read .zip, so the player may unpack one on request
 * (never done silently). Target: data_dir()/rom-cache/3ds/<key>/<image name>. */
std::string extracted_3ds_path(const GameEntry& e);   // the unpacked image if it exists, else ""
/* progress(done, total) returns false to cancel. On success *out_path is the image. Checks free disk space first. */
bool extract_3ds_zip(const GameEntry& e, const std::function<bool(uint64_t, uint64_t)>& progress,
                     std::string* out_path, std::string* err);
std::string extract_3ds_dir(const GameEntry& e);      // where it would go (for the confirm dialog)
uint64_t zip_3ds_unpacked_size(const GameEntry& e);   // size of the image inside the zip (central directory only), 0 if unknown

/* PlayStation games in .zip / .7z (issue == "extract", system PS1; identified by a Redump file name) also use those four calls: the whole archive
 * unpacks to data_dir()/rom-cache/ps1/<key>/ (cue + bins together) and extracted_3ds_path() returns the .cue/.chd/.iso to launch. */

/* GameCube discs in .7z / .zip (issue == "extract", system GC) use the same four calls above; they unpack with
 * Windows' tar.exe into data_dir()/rom-cache/gc/<key>/disc.<ext>. Pure helpers (exposed for tests): */
struct ArchiveEntry { std::string name; uint64_t size = 0; };
bool archive_entry_safe(const std::string& name);              // false for "..", absolute, drive-letter or control-char names
bool is_7z_file(const std::string& path);                      // 7z signature check
bool is_rar_file(const std::string& path);                     // RAR 4/5 signature check (read through Windows' bsdtar like 7z)
bool archive_sig_ok(const std::string& path);                  // .7z / .rar: signature matches; other extensions: true
bool archive_truncated(const std::string& path);               // 7z whose end header lies beyond the end of the file
std::vector<ArchiveEntry> parse_tar_listing(const std::string& text);   // "tar -tvf" output -> regular files
std::string gc_serial_for_name(const std::string& file_name);  // No-Intro style name -> catalog GC disc ID, "" if unknown

/* Why an archive that holds no recognizable game was skipped ("contains another archive ...", "damaged ...", "no game files ..."). */
std::string archive_skip_reason(const std::string& path);
/* Archives the LAST scan_library() skipped, with the reason (cached with the scan results). */
struct SkippedArchive { std::string path, reason; };
std::vector<SkippedArchive> scan_skipped_archives();

/* ---- unpacked-games cache (data_dir()/rom-cache) ----
 * Archives are unpacked ONCE (cartridge ROMs at scan time, discs on first Play) and reused until the archive changes.
 * The user's archive is never touched. Entries are evicted least-recently-used when a limit is set. */
uint64_t rom_cache_bytes();                          // disk space used by unpacked games
bool rom_cache_clear();                              // delete every unpacked game (the next scan unpacks cartridges again)
void rom_cache_set_limit(uint64_t bytes);            // 0 = unlimited (default 40 GB until the hub sets it)
uint64_t rom_cache_limit();
uint64_t rom_cache_trim(uint64_t max_bytes);         // evict LRU entries (not used in the last hour) until under max_bytes; returns bytes freed
void rom_cache_touch(const GameEntry& e);            // mark the unpacked copy of e as just used (call at launch)

/* Per-user library state, persisted to data_dir()/library.json. */
struct GameState {
    bool favorite = false;
    uint64_t last_played = 0;   // unix seconds
    uint64_t play_seconds = 0;
};
GameState game_state(const std::string& key);
void set_favorite(const std::string& key, bool fav);   // also mirrored into the synced account prefs (prefs.h) once prefs::set_vault_root() was called
std::vector<std::string> favorite_keys();               // every favorited key (sorted)
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
