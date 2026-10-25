#pragma once
/* Cheat database: per-game cheat lists for N64, GB, GBC, GBA and PS1.
 *
 * Sources (see scripts/build_cheats_db.py and THIRD_PARTY.md):
 *   - data/cheats_db.bin: libretro-database cht/ (pinned commit, CC BY-SA 4.0) converted to a compact zlib bundle.
 *   - data/cheats_featured.json: hand-curated headline cheats, merged in and ranked first:
 *       { "<No-Intro style game name>": [ {"desc":"...", "format":"gameshark|ar|codebreaker|gamegenie|raw",
 *                                           "codes":["8033B21E 0008", ...], "source":"url", "verified":true}, ... ], ... }
 *     The key is matched like a ROM name (region tags, "The", punctuation and word order are ignored; the region tag
 *     restricts the match). "verified": false entries are skipped.
 * Matching is by normalized game name + region (no CRC): the entry's catalog No-Intro name, title and file name are
 * all tried; the N64 serial (0x3B) / GBA code supply the region. */

#include <string>
#include <vector>

#include "cheats.h"
#include "library.h"

namespace hub {

/* Ordered result: master/enable codes first, then "Popular" (featured) cheats, then the rest (db order). */
struct CheatList {
    std::vector<cheats::Cheat> cheats;
    size_t masters = 0;    // cheats[0 .. masters): enable/master codes some games need first
    size_t featured = 0;   // cheats[masters .. masters+featured): the Popular ones
};

/* Directory holding cheats_db.bin / cheats_featured.json. Default: exe_dir() + "data/". Drops any loaded data. */
void cheats_db_set_dir(const std::string& dir);

CheatList cheats_list_for(const GameEntry& e);
/* Spec form: just the ordered cheats; *n_featured (optional) = masters + featured count (the prefix to show first). */
std::vector<cheats::Cheat> cheats_for(const GameEntry& e, size_t* n_prefix = nullptr);

/* Pieces, exposed for tests. */
std::string cheats_norm_key(const std::string& name);          // order-independent token key ("" if empty)
std::string cheats_region_of(const std::string& name);         // subset of "UEJ" (W/none -> "")
bool cheats_is_featured_desc(const std::string& desc);         // keyword ranking
bool cheats_is_master_desc(const std::string& desc);
size_t cheats_db_game_count();                                  // sections loaded (0 = bundle missing)

}  // namespace hub
