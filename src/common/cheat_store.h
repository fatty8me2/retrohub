#pragma once
/* Per-game cheat choices, stored in the save vault so they follow the player to every PC.
 *
 * File: <vault>\_settings\cheats.json  {"games":{"<game key>":{"on":["id",...],"custom":[{id,desc,format,codes}],
 *       "backup":"<name>","t":<unix>}}}
 * The cheat DATABASE (cheats_db.h) is read-only; this store only remembers which ids are on and the player's own codes.
 * Pure logic, no UI: rows (Popular / All / My codes), search, custom-code parsing + format auto-detect, the runner's
 * cheats file, online rules and the first-time save backup / restore. Covered by hubcheatstore_test. */
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cheats.h"

namespace cheatstore {

struct Custom {
    std::string id, desc;
    cheats::Format format = cheats::Format::Raw;
    std::vector<std::string> codes;
};

struct GameState {
    std::set<std::string> on;
    std::vector<Custom> custom;
    std::map<std::string, uint32_t> values;   // value cheats: id -> the number the player typed (cheats.h value_*)
    std::string backup;     // name of the backup made the first time cheats were enabled ("" = none yet)
    int64_t updated = 0;    // unix seconds of the last change (newer wins when two PCs merge)
};

class Store {
public:
    bool is_on(const std::string& game, const std::string& id) const;
    int on_count(const std::string& game) const;
    void set_on(const std::string& game, const std::string& id, bool on, int64_t now);
    const GameState* game(const std::string& game) const;
    /* Parses `codes_text` (one code per line), auto-detects the format for `target`. Returns the new id, or "" and *err. */
    std::string add_custom(const std::string& game, const std::string& desc, const std::string& codes_text,
                           cheats::Target target, int64_t now, std::string* err);
    /* Value cheats (placeholder digits): the number chosen for `id`. */
    bool value_of(const std::string& game, const std::string& id, uint32_t* out) const;
    void set_value(const std::string& game, const std::string& id, uint32_t v, int64_t now);
    void remove_custom(const std::string& game, const std::string& id, int64_t now);
    void set_backup(const std::string& game, const std::string& name, int64_t now);

    std::string to_json() const;
    bool from_json(const std::string& text);              // false (unchanged) on malformed input
    void merge_from(const Store& other);                  // per game: the entry with the newer `updated` wins
    bool load(const std::string& vault_dir);              // missing file = empty, true
    bool save(const std::string& vault_dir) const;
    static std::string file_path(const std::string& vault_dir);

private:
    std::map<std::string, GameState> g_;
};

/* ---- list rows ---- */
struct Row {
    cheats::Cheat cheat;    // enabled = current state
    bool featured = false;
    bool custom = false;
    int group = 1;          // 0 Popular, 1 All cheats, 2 My codes
    cheats::Target target = cheats::Target::N64;
    cheats::ValueSpec spec;     // spec.valid: a value cheat (cheat.codes keep the placeholders)
    uint32_t value = 0;         // the chosen number (spec.min until the player picks one)
    bool value_set = false;
};
struct DbItem { cheats::Cheat cheat; bool featured = false; };

/* Database items + the player's custom codes, state applied. Order: Popular, All (database order), My codes. */
std::vector<Row> build_rows(const Store& st, const std::string& game, const std::vector<DbItem>& db,
                            cheats::Target target = cheats::Target::N64);
/* The cheat as the runner / room sees it: a value cheat gets its number substituted for the placeholders. */
cheats::Cheat effective_cheat(const Row& r);
/* Case-insensitive substring filter on the description; an empty query keeps everything. */
std::vector<Row> filter_rows(const std::vector<Row>& rows, const std::string& query);

/* The file handed to the runner (--cheats): every row, enabled flags set. */
cheats::CheatSet runner_set(const std::vector<Row>& rows);
/* Only the enabled rows (what the host sends to the room). */
cheats::CheatSet enabled_only(const std::vector<Row>& rows);
std::vector<std::string> enabled_names(const std::vector<Row>& rows);

/* ---- custom codes ---- */
std::vector<std::string> split_codes(const std::string& text);   // lines, trimmed, ':' -> ' ', upper-case, no blanks
/* Format of one line for `target` ("8033B21E 00FF" N64 GameShark, "82003000 FFFF" GBA Action Replay,
 * "01FF5CD1" GB GameShark, "0AF-4E1-3C1" Game Genie). False if it fits none. */
bool detect_format(cheats::Target target, const std::string& line, cheats::Format* out);

/* ---- online ---- */
/* "" = fine; else why the host cannot launch with cheats on. Peers on a different/unknown RetroHub version may not
 * support cheats. Nothing to check when no cheat is enabled. */
std::string online_block(int enabled_count, const std::string& my_version, const std::vector<std::string>& peer_versions);
/* "Cheats on: A, B (+2 more)" ("" when none). */
std::string summary_line(const std::vector<std::string>& names, size_t max_names = 3);
/* The list a guest applies, from the host's launch message ("cheats" object); false if absent/malformed. */
bool extract_launch(const std::string& launch_json, cheats::CheatSet* out);
/* ",\"cheats\":{...}" for the launch message extra_json ("" when nothing is enabled). */
std::string launch_fragment(const cheats::CheatSet& enabled);

/* ---- save safety ---- */
/* Copies every file of `battery_dir` into <game dir>\cheat_backups\<stamp>\ (game dir = hub::saves::game_dir(), so it syncs with the game). *name = stamp. Never deletes.
 * Returns false (and *err) on a copy failure; an empty/missing battery dir still succeeds (nothing to back up). */
bool backup_save(const std::string& game_dir, const std::string& battery_dir, const std::string& stamp, std::string* err);
/* Copies the named backup back over battery_dir (overwrite). Before overwriting, the current files are backed up under
 * `<stamp>-before-restore` so a restore can be undone. Returns the number of files restored, -1 on error. */
int restore_save(const std::string& game_dir, const std::string& battery_dir, const std::string& name,
                 const std::string& undo_stamp, std::string* err);
bool has_backup(const std::string& game_dir, const std::string& name);

}  // namespace cheatstore
