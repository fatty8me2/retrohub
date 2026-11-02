#pragma once
/* ROM hacks and translations: everything that touches files but not the UI (Plan 4 H2-H4). Unit-tested in test_hack.cpp.
 * Identity and hack.json live in hack_logic.h; the patch engine in common/patcher.h; the dialogs and hooks in hack_ui.cpp. */
#include "hack_logic.h"
#include "library.h"
#include "patcher.h"

#include <functional>
#include <string>
#include <vector>

namespace hubui {
namespace hacks {

namespace patch = ::hub::patch;

/* A game that could be a patch's base. */
struct Cand {
    std::string key, title, path;
    hub::System system = hub::System::Unknown;
    uint64_t size = 0;
};
/* Library entries that can serve as a base: real files, not hacks, not native/co-op packages, not still-archived images. */
std::vector<Cand> candidates_from(const std::vector<hub::GameEntry>& lib);

/* The CRC32s a patch could name for one file. N64 files may be v64 / n64 byte order (patches assume z64), SNES dumps may carry a 512-byte copier header. */
struct ImageCrcs {
    uint64_t size = 0;
    uint32_t raw = 0;                                  // the file as it is
    bool has_z64 = false; uint32_t z64 = 0;            // N64: z64-order CRC (differs from raw only for v64 / n64 files)
    bool has_nohdr = false; uint32_t nohdr = 0; uint64_t nohdr_size = 0;   // SNES copier header (size % 1024 == 512) removed
};
bool image_crcs(const Cand& c, ImageCrcs* out, std::string* err);
/* N64 byte order of a file's first 4 bytes: 'z' z64 (big endian), 'v' v64 (byte-swapped), 'n' n64 (little endian), 0 unknown. */
char n64_order(const uint8_t* h4);

enum class FindStatus { Found, Choose, NotFound, WrongVersion };
struct FindResult {
    FindStatus status = FindStatus::NotFound;
    Cand base;                      // Found
    std::string prep;               // Found / chosen: how to prepare that base ("", "z64", "nohdr")
    std::vector<Cand> choices;      // Choose: same-size-range games, best name match first
    std::string message;            // NotFound / WrongVersion: plain words for the player
};
/* Finds the base game for a patch. UPS / BPS name it by source CRC (and size); IPS and VCDIFF do not, so the result is Choose with the
 * games that could fit (IPS: big enough for every record; VCDIFF: the source size when known), ranked by how much their title shares with
 * the patch file name. ips_extent: highest byte an IPS patch writes (ips_extent()), 0 = unknown / not IPS. */
FindResult find_base(const std::vector<Cand>& lib, const patch::Info& info, const std::string& patch_name, uint64_t ips_extent);
/* prep for a base the player chose by hand (IPS / VCDIFF): N64 files not in z64 order are normalised, everything else is patched as is. */
std::string default_prep(const Cand& c);
/* IPS: the size the base must have at least (max offset + length over the records, or the truncate size when larger). 0 if unreadable. */
uint64_t ips_extent(const uint8_t* d, size_t n);
uint64_t ips_extent_file(const std::string& patch_path);
/* Names ranked by words in common with `name` (lowercase words of 3+ characters). */
int name_score(const std::string& a, const std::string& b);

/* Writes the prepared base image for `prep` to tmp_path (z64: byte order normalised; nohdr: first 512 bytes dropped). */
bool prepare_source(const std::string& base_path, const std::string& prep, const std::string& tmp_path, std::string* err,
                    const std::function<bool(double)>& progress);

struct BuildResult {
    bool ok = false;
    std::string message;        // plain words
    uint32_t out_crc = 0;
    uint64_t out_size = 0;
};
/* base (prepared per prep) + patch -> out_path. Reports the CRC and size of the result. Never leaves a partial out_path. progress(0..1) false = cancel. */
BuildResult build_patched(const std::string& base_path, const std::string& prep, const std::string& patch_path, const std::string& out_path,
                          const std::function<bool(double)>& progress);

/* "hack.z64" / "hack.gba" ...: the file name of the patched image for a base file. */
std::string patched_name(hub::System s, const std::string& base_path);
/* Folder name inside rom-cache\hacks for a hack key. */
std::string cache_folder(const std::string& key);

/* ---- vault ---- */
struct Found {
    HackDef def;
    std::string dir;          // the patch folder: <vault>/<system>/<key folder>/patch
    std::string patch_path;   // dir + patch_file
};
/* Every <vault>/<system>/<folder>/patch/hack.json that decodes and whose patch file exists. */
std::vector<Found> read_vault(const std::string& vault_dir);
/* Appends the hack entries whose base game is in *out (placed right after the base). Hacks whose base is missing, and keys already present, are skipped. */
int append_hacks(const std::string& vault_dir, std::vector<hub::GameEntry>* out);
/* The library entry of one hack for its base. */
hub::GameEntry make_entry(const HackDef& d, const std::string& patch_path, const hub::GameEntry& base);
/* game_dir = hub::saves::game_dir(system, key): copies the patch to <game_dir>\patch and writes hack.json (atomic). */
bool install_hack(const std::string& game_dir, const HackDef& d, const std::string& patch_src, std::string* err);
/* Deletes <game_dir>\patch (only when it holds a hack.json). Saves next to it are not touched. */
bool remove_hack_files(const std::string& game_dir);
std::string load_def_next_to(const std::string& patch_path, HackDef* out);   // "" ok, else why not

/* Ribbon text for a hack entry: "HACK" or "TRANSLATION". */
std::string ribbon_for(const HackDef& d);

}  // namespace hacks
}  // namespace hubui
