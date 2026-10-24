#pragma once
/* PlayStation 1 support that does not need the emulator: disc image probing (serial from SYSTEM.CNF), the Redump serial -> title
 * catalog, .cue/.m3u parsing and BIOS file recognition. See docs/EMULATORS.md ("PlayStation"). */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hub {

/* Generated at configure time from Beetle PSX's database/psx_games.inc (src/beetle_psx/gen_ps1_db.cmake). */
struct Ps1DbRow { const char* serial; const char* title; };
extern const Ps1DbRow kPs1Db[];
extern const size_t kPs1DbCount;

namespace ps1 {

/* "SLUS_006.94", "slus-00694", "SLUS 00694;1", "cdrom:\\SLUS_006.94;1" -> "SLUS-00694"; "" if it is not a Sony-style serial. */
std::string normalize_serial(const std::string& s);
/* Boot serial from SYSTEM.CNF text ("BOOT = cdrom:\SLUS_006.94;1"), normalized; "" if absent. */
std::string serial_from_system_cnf(const std::string& text);
/* Redump title for a serial ("Crash Bash (USA)"), "" if unknown. */
std::string title_for_serial(const std::string& serial);
/* First catalog serial whose title equals this file name stem (case-insensitive, ignores "(Disc N)" only when exact match fails). */
std::string serial_for_name(const std::string& file_stem);
/* Disc number from "(Disc 2)" in a title/file name, 0 if none. */
int disc_number(const std::string& name);
/* Region from a serial prefix: 'U' North America, 'E' Europe, 'J' Japan, 0 unknown. */
char serial_region(const std::string& serial);

/* Random-access reader of 2048-byte user-data sectors. */
struct SectorReader {
    virtual ~SectorReader() = default;
    virtual bool read(uint32_t lba, uint8_t* out2048) = 0;
};
/* Walks the ISO 9660 root of a disc and returns the boot serial. *is_iso is set when a primary volume descriptor was found. */
std::string serial_from_disc(SectorReader& r, bool* is_iso = nullptr, bool* has_boot = nullptr);   // has_boot: SYSTEM.CNF with a BOOT line (homebrew has no serial)

struct CueFile { std::string file; std::string mode; };       // FILE "x.bin" BINARY / TRACK 01 MODE2/2352
/* Files referenced by a cue sheet in order, with the mode of the first track of each file. */
std::vector<CueFile> parse_cue(const std::string& text);
/* Non-comment lines of an .m3u (paths as written). */
std::vector<std::string> parse_m3u(const std::string& text);

/* What a disc-like file is. serial may be empty (homebrew, unreadable); ok false = not a PlayStation disc/exe at all. */
struct DiscInfo { bool ok = false; std::string serial; std::string title_hint; };
/* Probes .cue .bin .img .iso .chd .pbp .m3u .exe by content (reads a few sectors; for .chd decodes the first hunks). */
DiscInfo probe_file(const std::string& path_utf8);
/* Files a launch needs next to path_utf8: for a .cue its bins, for an .m3u every disc and its files (absolute UTF-8). */
std::vector<std::string> referenced_files(const std::string& path_utf8);

/* ---- BIOS ---- */
struct BiosInfo { const char* file; const char* md5; char region; const char* desc; };   // lowercase md5
const BiosInfo* bios_known(const std::string& md5_lower);
const std::vector<BiosInfo>& bios_list();
/* Name the core looks for in its system dir for a region ('U' scph5501.bin, 'E' scph5502.bin, 'J' scph5500.bin). */
const char* bios_core_name(char region);
/* A BIOS candidate: file name is scph*.bin-like and size 524288 (or the 1 MB SCPH-5903). */
bool bios_name_plausible(const std::string& file_name);

struct BiosFound { std::string path; std::string md5; const BiosInfo* known = nullptr; char region = 0; };
/* Looks in each directory (non-recursive) for BIOS files; known ones by MD5, plausible unknown ones flagged known == nullptr. */
std::vector<BiosFound> find_bios(const std::vector<std::string>& dirs_utf8);
/* Where to look for BIOS files, best first: the user's pick (a file's folder or a folder), the staging dir, each game folder with its BIOS /
 * "bios" / "System" siblings (the folder itself, its BIOS subfolder, and BIOS next to it up to two levels up: Desktop\Everything\BIOS beside
 * Desktop\Everything\GamesPS1), then <home>\Desktop\Everything\BIOS, <exe>\system, <exe>ios and RetroArch's system folder. Pure. */
std::vector<std::string> bios_search_dirs(const std::vector<std::string>& rom_folders, const std::string& user_pick, const std::string& staging_dir,
                                          const std::string& home, const std::string& exe_dir, const std::string& appdata);
/* Copies the best BIOS per region into `system_dir` under the names the core looks for (a region with no BIOS of its own gets the best one found: any BIOS boots any region);
 * returns how many regions had a BIOS of their own. */
int stage_bios(const std::vector<BiosFound>& found, const std::string& system_dir);

} // namespace ps1
} // namespace hub
