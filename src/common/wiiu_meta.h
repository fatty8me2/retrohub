#pragma once
/* Wii U (Cemu) helpers that need no UI and no process handling:
 *  - ZArchive (.wua) reader: root titles (base / update / DLC) and single files such as <title>/meta/meta.xml (zstd blocks)
 *  - meta.xml parsing (title id, long name, product code, region)
 *  - installed updates / DLC in Cemu's mlc01\usr\title\0005000e / 0005000c
 *  - .wud/.wux key check (keys.txt), save folders (base + update titles, every account), Cemu game-profile INIs
 * Unit-tested by hub_emu_test. */

#include <cstdint>
#include <string>
#include <vector>

#include "emu_formats.h"
#include "emu_saves.h"

namespace hub {

/* ---------------- meta.xml ---------------- */
struct WiiuMeta {
    uint64_t title_id = 0;
    std::string longname;        // English long name when present (else the first language that has one); whitespace collapsed
    std::string publisher;
    std::string product_code;    // "WUP-N-ALZE"
    char region = 0;             // last letter of the product code's 4-char id: E (USA) P (Europe) J (Japan) ... or 0
    std::string game_id4;        // "ALZE"
};
bool parse_wiiu_meta_xml(const std::string& xml, WiiuMeta* out);
/* "USA" / "Europe" / "Japan" / "" for a region letter. */
const char* wiiu_region_name(char region_letter);

/* ---------------- .wua (ZArchive) ---------------- */
struct WuaTitle {
    uint64_t title_id = 0;
    std::string dir;             // root directory name, e.g. "0005000e1019e500_v81"
    int version = -1;            // from "_v<N>" (-1 when absent)
    enum Kind { Base, Update, Dlc, Other } kind = Other;
};
std::vector<WuaTitle> wua_titles(const ReadAtFn& read, uint64_t file_size);
/* Reads one file ("<root dir>/meta/meta.xml", case-insensitive) fully decompressed; false when absent/too big/corrupt. */
bool wua_read_file(const ReadAtFn& read, uint64_t file_size, const std::string& path, std::string* out, size_t max_bytes = 1u << 20);
/* Base title's meta.xml (else the first title that has one). */
bool wua_read_meta(const ReadAtFn& read, uint64_t file_size, WiiuMeta* out);

/* ---------------- installed updates / DLC (Cemu mlc01) ---------------- */
struct WiiuAddons {
    bool update = false;  int update_version = -1;
    bool dlc = false;     int dlc_version = -1;
    bool update_in_archive = false, dlc_in_archive = false;   // .wua that bundles them
    int archive_update_version = -1;
};
/* app.xml <title_version>: "0051" -> 81 (-1 when unreadable). */
int parse_app_xml_version(const std::string& xml);
WiiuAddons wiiu_installed_addons(uint64_t base_title_id, const std::string& mlc, const FsProbe& fs);
void wiiu_addons_from_wua(const std::vector<WuaTitle>& titles, WiiuAddons* a);
/* One short line for the details page, e.g. "Update v81 (installed), DLC (installed)" or "No update or DLC installed in Cemu". */
std::string wiiu_addons_text(const WiiuAddons& a);

/* ---------------- .wud / .wux keys ---------------- */
struct WiiuKeys {
    std::string found_path;      // keys.txt that exists ("" when none)
    int key_lines = 0;           // lines that look like a 32-hex-digit key
    std::string expected_path;   // where Cemu looks first: <user data>\keys.txt
};
WiiuKeys wiiu_keys_check(const std::string& user_data, const std::string& exe_dir, const FsProbe& fs);
/* "" when nothing is wrong. Never contains key material. */
std::string wiiu_keys_message(const WiiuKeys& k, const std::string& container);

/* ---------------- saves ---------------- */
/* Relative save folders to sync for a base title: its own (usr/save/00050000/<low>) and its update title's (usr/save/0005000e/<low>). */
std::vector<std::string> wiiu_save_rels(uint64_t base_title_id);
/* Account folders under <save>\user (persistent ids such as 80000001; "common" excluded), sorted. */
std::vector<std::string> wiiu_save_accounts(const std::string& title_save_dir);

/* ---------------- Cemu game profiles (gameProfiles\<title id>.ini) ---------------- */
std::string wiiu_profile_file_name(uint64_t title_id);   // "000500001019e500.ini" (lower case, as Cemu writes it)
/* Section/key lookup; "" when absent. */
std::string ini_get(const std::string& text, const std::string& section, const std::string& key);
std::string ini_put(const std::string& text, const std::string& section, const std::string& key, const std::string& value);
/* Effective value: user override file wins over Cemu's shipped gameProfiles\default\<id>.ini. */
std::string wiiu_profile_value(const std::string& default_ini, const std::string& user_ini, const std::string& section, const std::string& key);
/* Choice values for the per-game CPU mode setting. */
const char* wiiu_cpu_mode_ini(const std::string& choice);   // "single-rec" -> "Singlecore-Recompiler"; "" for "game"/unknown

}  // namespace hub
