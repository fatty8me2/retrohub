#pragma once
/* Damaged-member repair for Lichii's NUP packages (Switch / Switch 2). UI-free. Reads the NUP, never writes it.
 *
 * Why: some shared v1.1.1 copies of The Legend of Zelda: Ocarina of Time (0400486024DEC000) carry .zs members whose first bytes are zeroed
 * (no zstd frame header). The game cannot load such a file, so it never builds the model that uses it: whole rooms are invisible but solid.
 *   romfs/Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs           the Water Temple rooms
 *   romfs/Tex/CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs  the fairy fountain caves (Land_FairyFountain_Base_A_01)
 *   romfs/Tex/RBake_Scene_GanonStage_G_2_1_d0_t0.bntx.zs        a Ganon's Castle light bake
 * (Lichii issues #420 #453 #487; the owner's copy, verified 2026-11-09.) Lichii PR #267 (open) serves a sibling from inside the emulator.
 * RetroHub does the same from outside, for every Lichii / Fritchii build, as a LayeredFS mod in the emulator's portable user folder:
 *   <user_dir>\load\<TITLE ID: 16 upper-case hex>\RetroHub damaged-texture fix\romfs\<path inside the romfs>   = the stand-in's bytes
 * Lichii's PatchManager::ApplyLayeredFS layers every load\<tid>\<mod>\romfs folder over the game's RomFS at boot (NUP games too:
 * loader/nup.cpp ReadRomFS -> RomFSFactory -> PatchRomFS(apply_layeredfs = true)). The folder name is the one the owner's hand-made repair
 * used on 2026-11-09, so that folder is adopted (rewritten byte-identical, then listed in the manifest).
 *
 * NUP layout (Lichii src/core/file_sys/nup_container.cpp; all little-endian):
 *   superblock 0x80: "NUP\x01", u16 header_size, u16 version, u64 toc_offset @0x08, u64 toc_size @0x10, u8[32] toc_sha256 @0x18,
 *                    u64 blob_offset @0x38, u64 file_size @0x40 (== real size), u32 package_count @0x48, u32 flags @0x4C (must be 0)
 *   TOC (SHA-256 of the toc_size bytes == toc_sha256): header 0x30 = u32 "TOC\x01" (0x01434F54), u32 package_count, u64 dir_count,
 *       u64 file_count, u64 pool_offset, u64 pool_size, u64 0; then package records (0x40 each: u64 title_id, u32 root_dir, u32 exefs_dir,
 *       u32 romfs_dir (0xFFFFFFFF = none), u32 version, u8 kind (0 base, 1 update, 2 add-on), u8 flags (1 = standalone update), ...),
 *       dir records (0x18: u32 name_offset, u32 name_length, u32 first_child, u32 child_count, u64 0), file records (0x50: u32 name_offset,
 *       u32 name_length, u64 size, u64 extent_offset (absolute, >= blob_offset, 16-aligned), u64 extent_size, u8[32] sha256 of the
 *       uncompressed content, u8 codec (0 raw, 1 zstd frame), u8 codec_flags (0), ...), then u32 children up to pool_offset
 *       (child < dir_count: a dir; else file index child - dir_count), then the UTF-8 name pool (names relative to pool_offset).
 *   A size-0 file has extent 0/0. Every bound is checked like nup_container.cpp Parse(); anything malformed -> Scan.ok = false, never a crash.
 *
 * Merged RomFS (loader/nup.cpp ReadRomFS): the base package's romfs (first kind-0 package) with the best update's romfs over it
 * (FindBestUpdatePackage: the kind-1 package with the highest version); update paths win, directories merge. A standalone update alone.
 *
 * Detection (exactly PR #267): a member is damaged when its name ends in ".zs", its codec is zstd (1) and the first 4 bytes of its
 * decompressed content are not the zstd frame magic 28 B5 2F FD (or it does not decompress). Only codec-1 members are checked: a game .zs is
 * already compressed, so the NUP writer only recompresses one that shrinks, which a zeroed range does (the owner's OoT copy: 1,083 of
 * 102,185 .zs members, at most 128 KiB read from each, about 100 MB in all).
 * Stand-in (PR #267 FindStandIn): among the files of the SAME directory of the merged RomFS, those whose name ends with the damaged name's
 * last '_' token (e.g. "Nrm.bntx.zs") with a '_' right before it; the longest common name prefix wins, then the smallest uncompressed-size
 * difference; the candidate must itself start with the zstd magic (any codec). No candidate -> the member stays as it is (unrepaired).
 * Owner's copy result: Gate_M_01_Nrm <- Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs, Soil_FairyFountain_Floor_D_Nrm <-
 * CmnTex_Map_Sand_FairyFountain_Sand_A_Nrm.bntx.zs, GanonStage_G_2_1_d0_t0 <- RBake_Scene_GanonStage_G_2_2_d0_t0.bntx.zs.
 *
 * Manifest: <mod folder>\retrohub-repair.txt (LayeredFS reads only romfs / romfs_ext / romfslite, so it is ignored by the emulator):
 *   line 1  "RetroHub damaged-file repair 1"
 *   line 2  "nup <file size> <last-write time, Windows FILETIME ticks> <toc sha256, 64 lower-case hex> rule <kRuleVersion>"
 *   then    "repair <sha256 of the written file, 64 hex> <size>\t<damaged path>\t<stand-in path>"   (paths inside the romfs, '/' separated)
 * Written last (temp file + rename), after every stand-in file is in place.
 *
 * Paths in this API are UTF-8; files are opened with the wide Windows APIs. */

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hub::nuprepair {

constexpr const char* kModFolder = "RetroHub damaged-texture fix";
constexpr const char* kManifestName = "retrohub-repair.txt";
constexpr int kRuleVersion = 1;   // bump when detection or the stand-in choice changes: every manifest is then stale and the copy is rescanned

struct Member {
    std::string path;    // inside the merged romfs, '/' separated: "Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"
    uint64_t size = 0;   // uncompressed size
};

struct Repair {
    Member damaged;
    Member stand_in;     // stand_in.path empty: no stand-in found (left as it is)
};

struct Scan {
    bool ok = false;               // false: not a NUP, unreadable, malformed, or TOC hash mismatch (error says which). Nothing to repair then.
    std::string error;
    uint64_t title_id = 0;         // the title id of the package that supplies the program (base if any, else the best update) = load\<tid>
    uint64_t file_size = 0;
    uint64_t mtime = 0;            // last-write time of the NUP, FILETIME ticks
    std::string toc_sha256;        // 64 lower-case hex, from the superblock (identifies the copy)
    uint64_t checked = 0;          // codec-1 .zs members whose start was decompressed
    std::vector<Repair> repairs;   // sorted by damaged.path; includes members with no stand-in
};

/* Full scan: superblock, TOC (SHA-256 checked), then up to 128 KiB of each codec-1 .zs member of the merged romfs, plus the starts of the
 * candidate stand-ins. cancel (optional) is polled between members; a cancelled scan returns ok = false, error "cancelled". */
Scan scan(const std::string& nup_path, const std::atomic<bool>* cancel = nullptr);

/* The uncompressed bytes of merged-romfs member `romfs_path`: raw extents copied, zstd extents decompressed; the SHA-256 of the result must
 * equal the TOC's, else false with err. */
bool extract(const std::string& nup_path, const std::string& romfs_path, std::vector<uint8_t>* out, std::string* err);

struct Applied {
    bool ok = false;
    std::string error;
    int written = 0;      // stand-in files written this time (new or different)
    int kept = 0;         // already in place with the right SHA-256 (untouched)
    int removed = 0;      // files a previous manifest listed that are no longer needed (deleted)
    int unrepaired = 0;   // damaged members without a stand-in
};

/* Makes <user_dir>\load\<tid>\<kModFolder> match `scan` (which must be ok): each stand-in is extracted (extract()) and written under
 * romfs\<damaged path> (temp file + rename, only when missing or different), files listed by an earlier manifest but no longer needed are
 * deleted, then the manifest is written (temp + rename). With no repairs at all, RetroHub's listed files and the manifest are deleted, and
 * then the empty directories up to and including the mod folder. Never deletes or changes anything the old manifest does not list (other
 * mods, the player's own files), except overwriting a stand-in path it is about to list. user_dir = the emulator's portable user folder
 * (<install dir>\user). */
Applied apply(const std::string& nup_path, const Scan& scan, const std::string& user_dir);

/* The setting is off: deletes only what the manifest of load\<tid>\<kModFolder> lists, the manifest, then the folder if it is empty.
 * Returns the number of files deleted. No manifest -> nothing is touched (a hand-made folder stays). */
int remove(const std::string& user_dir, uint64_t title_id);

struct ManifestEntry {
    std::string sha256;          // of the written stand-in file
    uint64_t size = 0;
    std::string damaged_path;    // romfs path that is replaced
    std::string stand_in_path;   // romfs path its bytes came from
};
struct Manifest {
    uint64_t file_size = 0, mtime = 0;
    std::string toc_sha256;
    int rule = 0;
    std::vector<ManifestEntry> entries;
};
/* Reads load\<tid>\<kModFolder>\retrohub-repair.txt under user_dir. false: missing or malformed. No NUP reads (the game page uses this). */
bool read_manifest(const std::string& user_dir, uint64_t title_id, Manifest* out);

/* Launch-time entry point (launch.cpp: Lichii and Fritchii, .nup games only). enabled = false -> remove(). Otherwise cheap when nothing
 * changed: reads the superblock, the TOC header + package records (for the title id), the manifest, and checks each listed file's size and
 * SHA-256; scan() + apply() only when the manifest is missing, for another copy (size / time / TOC hash), another rule version, or a listed
 * file is missing or different. state_dir (RetroHub's own folder, e.g. <data dir>\nup-repair; never one of the emulator's): when non-empty and a scan
 * finds no repair that has a stand-in (none at all, or only unrepairable ones), "<state_dir>\nup-repair-<TITLE ID 16 hex>.txt" is written (atomic) holding
 * the manifest's identity line ("nup <size> <mtime> <toc sha256> rule <kRuleVersion>") and "path <nup path>"; with no mod-folder manifest, a matching record
 * is the fast path too (scanned = false), so a healthy copy is scanned once, not at every launch. A copy that later gets repairs, or the setting off, drops
 * the record. Never throws; errors only end up in summary (the launch goes on regardless). */
struct Ensure {
    bool scanned = false;   // a full scan ran
    Scan scan;              // valid when scanned
    Applied applied;        // valid when scanned (or when enabled = false: removed holds the count)
    std::string summary;    // one line for the log: "nup repair: 3 repaired (0 written, 3 kept), 1083 checked" / "unchanged" / "off: 3 removed" / "error: ..."
};
Ensure ensure(const std::string& nup_path, const std::string& user_dir, bool enabled, const std::string& state_dir = "");

/* Plain words for the game page from a manifest: "" when it lists nothing. Names the places it knows (Water Temple, fairy fountains,
 * Ganon's Castle) from the damaged paths, else says "some rooms". */
std::string page_text(const Manifest& m);

}  // namespace hub::nuprepair
