#pragma once
/* Pal Park / DS Slot-2 (Plan 4 Wave 3 X3): pure logic, unit-tested (test_palpark.cpp). No UI, no files except the JSON text round trip.
 *
 * A DS game that can use the GBA slot (Pokemon Diamond / Pearl / Platinum / HeartGold / SoulSilver: Pal Park) gets a "GBA cartridge (Slot-2)" row in
 * its details. The player picks one of their GBA games (Gen 3 Pokemon first) or None; the choice is stored per DS game. Default: None, so nothing
 * changes for anyone who never touches it. Launch passes retrorun --slot2-rom <gba> [--slot2-save <that game's exact vault battery file>]. */
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "json.h"

namespace hubui {
namespace palpark {

/* DS games (by catalog id) that read the GBA slot. Add an id here to give another game the row (Pokemon Dash, Castlevania DoS, ...: the catalog
 * has no entry for them yet; the id is the same one games.cpp uses). */
inline bool ds_uses_slot2(const std::string& catalog_id) {
    static const char* ids[] = {"pokediamond", "pokepearl", "pokeplatinum", "pokeheartgold", "pokesoulsilver"};
    for (const char* i : ids) if (catalog_id == i) return true;
    return false;
}

/* Gen 3 Pokemon (the games Pal Park accepts), in release order. -1 = not one of them. */
inline int gen3_rank(const std::string& catalog_id) {
    static const char* ids[] = {"pokeruby", "pokesapphire", "pokeemerald", "pokefirered", "pokeleafgreen"};
    for (int i = 0; i < 5; i++) if (catalog_id == ids[i]) return i;
    return -1;
}

struct Gba {
    std::string key, title, catalog_id, path, save_stem;
};

inline std::string lower_ascii(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

/* The player's GBA games in menu order: Gen 3 Pokemon first (release order), then the rest A-Z. Returns indexes into `all`. Empty key/path skipped. */
inline std::vector<size_t> order_choices(const std::vector<Gba>& all) {
    std::vector<size_t> v;
    for (size_t i = 0; i < all.size(); i++) if (!all[i].key.empty() && !all[i].path.empty()) v.push_back(i);
    std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {
        int ra = gen3_rank(all[a].catalog_id), rb = gen3_rank(all[b].catalog_id);
        if ((ra >= 0) != (rb >= 0)) return ra >= 0;
        if (ra >= 0) return ra < rb;
        return lower_ascii(all[a].title) < lower_ascii(all[b].title);
    });
    return v;
}

inline const Gba* find_gba(const std::vector<Gba>& all, const std::string& key) {
    if (key.empty()) return nullptr;
    for (const Gba& g : all) if (g.key == key) return &g;
    return nullptr;
}

/* File stem retrorun uses for a GBA game's battery file: <save_stem>.srm, else the stem of the ROM file name (zipped ROM: the inner name). */
inline std::string file_stem(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    size_t d = n.rfind('.');
    return d == std::string::npos || d == 0 ? n : n.substr(0, d);
}
inline std::string srm_stem(const Gba& g) { return g.save_stem.empty() ? file_stem(g.path) : g.save_stem; }

/* Which file in the GBA game's battery folder is its save: <stem>.srm (any case); else, like retrorun, the one lone .srm (the same game kept under
 * another file name on this PC); else "" (no save yet). `names` = file names in the folder. */
inline std::string pick_save_file(const std::vector<std::string>& names, const std::string& stem) {
    const std::string want = lower_ascii(stem + ".srm");
    std::vector<std::string> srms;
    for (const std::string& n : names) {
        std::string l = lower_ascii(n);
        if (l == want) return n;
        if (l.size() > 4 && l.compare(l.size() - 4, 4, ".srm") == 0) srms.push_back(n);
    }
    return srms.size() == 1 ? srms[0] : std::string();
}

/* The retrorun flags (each starting with a space), or "" when there is no GBA ROM or a path would break the command line. `save` may be "" (no save yet). */
inline std::string build_args(const std::string& gba_rom, const std::string& save, const std::string& backup_dir = std::string()) {
    if (gba_rom.empty() || gba_rom.find('"') != std::string::npos || save.find('"') != std::string::npos || backup_dir.find('"') != std::string::npos) return std::string();
    std::string a = " --slot2-rom \"" + gba_rom + "\"";
    if (!save.empty()) a += " --slot2-save \"" + save + "\"";
    if (!save.empty() && !backup_dir.empty()) a += " --slot2-backup-dir \"" + backup_dir + "\"";   // only with a save: no save, nothing to back up
    return a;
}

/* Where retrorun keeps the backups of this GBA game's save before a Pal Park run: <data dir>\palpark_backups\<game key as a folder name>, like Transfer Pak's
 * tpak_backups, OUTSIDE the synced vault (a backup inside the vault would use up the cloud quota and be copied to every PC). "" when either part is empty. */
inline std::string backup_dir_for(const std::string& data_dir, const std::string& gba_key) {
    std::string d = data_dir, k = gba_key;
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    for (char& c : k) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) c = '_';   // keys look like "gba:axve-1234": no ':' in a folder name
    if (d.empty() || k.empty()) return std::string();
    return d + "\\palpark_backups\\" + k;
}

/* ---- the per-DS-game choice: { "slot2": { "<ds game key>": "<gba game key>" } } in hub::data_dir()\palpark.json ---- */
typedef std::map<std::string, std::string> Choices;

inline Choices parse_choices(const std::string& text) {
    Choices c;
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) return c;
    const json::Value* m = v.get("slot2");
    if (!m || !m->is_object()) return c;
    for (const auto& kv : m->o) if (kv.second.is_string() && !kv.second.s.empty() && !kv.first.empty()) c[kv.first] = kv.second.s;
    return c;
}

inline std::string write_choices(const Choices& c) {
    json::Value root = json::make_object(), m = json::make_object();
    for (const auto& kv : c) if (!kv.second.empty()) m.o.push_back({kv.first, json::make_string(kv.second)});
    root.o.push_back({"slot2", m});
    return json::stringify(root);
}

/* "None" is stored as no entry at all. */
inline void set_choice(Choices* c, const std::string& ds_key, const std::string& gba_key) {
    if (gba_key.empty()) c->erase(ds_key); else (*c)[ds_key] = gba_key;
}
inline std::string get_choice(const Choices& c, const std::string& ds_key) {
    auto it = c.find(ds_key);
    return it == c.end() ? std::string() : it->second;
}

}  // namespace palpark
}  // namespace hubui
