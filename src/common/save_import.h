#pragma once
/* "Bring your saves" (Plan 4 Wave 3 L4): find the save files other emulators left on this PC, match them to library games and
 * convert them into exactly what the RetroHub runners read from the save vault (docs/SAVES.md). Pure logic: no UI, no network,
 * no cloud. The hub side (src/hub/save_import_ui.cpp) shows the preview, then calls apply() and queues the cloud push.
 *
 * What each runner reads from <vault>\<system>\<key>\battery\ (so what apply() writes):
 *   N64 (n64run = mupen64plus core)  "<name>.eep" (always 2048 bytes), ".sra" (32 KB), ".fla" (128 KB), ".mpk" (all four
 *                                     Controller Paks in ONE 128 KB file). <name> follows the core's SaveFilenameFormat 1:
 *                                     "<GoodName cut to 32>-<first 8 of the ROM MD5>" (see n64_save_base).
 *   GB/GBC/GBA/NES/SNES/DS/PS1        "<ROM file stem>.srm" (retrorun; the stem is GameEntry::save_stem or the ROM path's stem)
 *   GameCube (Dolphin GCI folder)     "GCI\*.gci" (one file per save; a raw card is split into .gci files)
 *   PlayStation 2 (PCSX2)             "memcards\RetroHub\<safe key>\Mcd001.ps2" (the game's own 8 MB card)
 * Only in-game saves move: another emulator's save STATES are a different format and are never imported. */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hub::saveimport {

enum class Source { RetroArch, Project64, Dolphin, PCSX2, MGBA, VBA, MelonDS, NextToRom, Picked };
const char* source_name(Source s);   // "RetroArch", "Project64", ...

/* ---- where to look ---- */
struct Env {   // all UTF-8, no trailing slash needed
    std::string appdata, localappdata, userprofile, documents, programfiles, programfiles_x86, system_drive;
    std::string own_data_dir;   // RetroHub's own folder: never offered as a source
};
struct Folder { Source src; std::string dir; };
std::vector<Folder> detect_folders(const Env& env);

/* retroarch.cfg: savefile_directory / savestate_directory. "default" or ":\x" (relative to the RetroArch folder) are resolved
 * against `ra_dir`. Empty when unset. */
struct RaCfg { std::string savefile_dir, savestate_dir; bool saves_in_content_dir = false; };
RaCfg parse_retroarch_cfg(const std::string& cfg_text, const std::string& ra_dir);
/* mGBA config.ini: savegamePath ("" when unset). */
std::string parse_mgba_savegame_path(const std::string& ini_text);

/* ---- what was found ---- */
struct Found {
    Source src = Source::Picked;
    std::string path;      // absolute UTF-8
    std::string name;      // file name
    std::string ext;       // lower case with dot: .srm .sav .eep .sra .fla .mpk .gci .raw .gcp .mcd .ps2
    int64_t size = 0;
    int64_t mtime = 0;     // unix seconds
};
std::vector<Found> scan_folder(const Folder& f, size_t max_files = 3000);
/* One file the player picked by hand (any of the extensions above). false when it is not a save file we know. */
bool found_from_path(const std::string& path, Source src, Found* out);
/* Files of one game's ROM folder ("next to the ROM": <stem>.srm / .sav / .eep ... and a "saves" sub folder). */
std::vector<Found> scan_next_to_rom(const std::string& rom_path, const std::string& stem);
/* RetroArch save states in savestate_dir (counted so the preview can say they are left alone). */
int count_files_with_ext(const std::string& dir, const std::string& ext_prefix);

/* ---- library games ---- */
struct GameRef {
    std::string key;         // GameEntry::key
    std::string system;      // system_short: n64 gb gbc gba nes snes nds ps1 gc ps2 ...
    std::string title;
    std::string save_stem;   // what retrorun names the .srm (never empty for retrorun systems)
    std::string rom_path;    // for the N64 header name
    std::string serial;      // GC disc id / PS2 serial when known
};
std::string pcsx2_safe_key(const std::string& key);   // identical to hubemu::pcsx2_safe_key (emulators.cpp)

/* ---- matching ---- */
enum class How { Stem, Title, Content, Beside };
const char* how_name(How h);
std::string norm_name(const std::string& s);   // "Pokemon - Emerald (USA) [!]" -> "pokemonemerald"
std::string strip_save_suffix(const std::string& stem, int* controller = nullptr);   // "Game_Cont_2", "Game-3A67D998" -> "Game"

/* ---- rows ---- */
struct OutFile { std::string rel; std::string data; };   // rel = path inside the game's battery folder, '/' separators

struct Row {
    std::vector<Found> files;      // usually one; a Dolphin GCI group has several
    size_t game = 0;               // index into the games vector
    How how = How::Stem;
    bool ambiguous = false;        // more than one library game fits: not pre-selected
    int controller = 0;            // N64 .mpk from Project64: 0-based controller of the file
    std::string gc_game;           // Dolphin raw card: the 4-char game code this row takes from the card ("" = all of a gci file)
    std::string problem;           // non-empty: cannot be imported (shown greyed with the reason)
    std::vector<std::string> out_rel;   // vault files this row writes
    // vault side
    bool vault_has = false;
    int64_t vault_mtime = 0, vault_size = 0;
    bool same_as_vault = false;
    int64_t src_mtime = 0, src_size = 0;
    enum Newer { NoVault, SourceNewer, VaultNewer, Same } newer = NoVault;
    bool selected = false;
    Source src() const { return files.empty() ? Source::Picked : files[0].src; }
};

struct Ctx {
    std::string n64_ini;                                           // text of mupen64plus.ini ("" if unavailable)
    std::function<std::string(const GameRef&)> battery_dir;        // <vault>\<system>\<key>\battery (with or without trailing slash)
};

/* Matches `found` to `games` (stem first, then title; GameCube cards by game code, PS2 cards by serial) and fills the vault side
 * and the default selection. Nothing is selected when the vault already holds a newer save, or when the match is ambiguous.
 * When several rows would write the same vault file only the newest source is pre-selected. Rows that cannot be converted keep
 * a `problem`. Reads the source files, so call it off the UI thread. */
std::vector<Row> build_rows(const std::vector<Found>& found, const std::vector<GameRef>& games, const Ctx& ctx);

/* The files a row would write (reads the source). false + *why when it cannot (wrong size, nothing in it, unknown N64 name...). */
bool convert(const Row& r, const GameRef& g, const Ctx& ctx, std::vector<OutFile>* out, std::string* why);

struct ApplyReport {
    int written = 0, backed_up = 0, unchanged = 0;
    bool ok = true;
    std::string error;
    std::vector<std::string> wrote;   // vault paths
};
/* Writes the converted files into the game's battery folder. An existing different file is copied to
 * <vault_root>\_conflicts\<system>\<key>\battery\<rel>.import.<yyyymmdd-hhmmss> FIRST; if that copy fails nothing is overwritten.
 * Writes are atomic (temp + rename) and stamp "now" as the file time, so the imported save is the newest everywhere. */
ApplyReport apply_row(const Row& r, const GameRef& g, const Ctx& ctx, const std::string& vault_root, int64_t now_unix);

/* ---- format helpers (exposed for tests) ---- */
std::string ini_goodname(const std::string& ini_text, const std::string& md5_upper);   // "" when unknown
std::string n64_header_name(const std::string& first_bytes);   // from the first 0x34 bytes of any byte order; "" when unreadable
/* File base name (no extension) mupen64plus uses for this ROM. `have_old(goodname)` says whether "<goodname>.<ext>" already exists
 * (the core keeps using that old name). "" when it cannot be known. */
std::string n64_save_base(const std::string& md5_upper, const std::string& goodname, const std::string& header_name);
struct GcEntry { std::string gamecode, maker, filename; int first = 0, count = 0; size_t off = 0; };
bool gc_card_entries(const std::string& raw, std::vector<GcEntry>* out);
bool gc_card_export_gci(const std::string& raw, const GcEntry& e, std::string* gci);
std::string blank_mempak(const std::string& seed);   // 32 KB formatted Controller Pak (what the core makes for a new file)
bool mempak_has_notes(const std::string& pak32k);

}  // namespace hub::saveimport
