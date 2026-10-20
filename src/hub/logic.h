#pragma once
/* Pure (UI/OS independent) logic for RetroHub: tiny JSON, settings, lobby rules. */

#include <map>
#include <string>
#include <vector>

namespace hubui {

/* ---- minimal JSON ---- */
struct Json {
    enum Type { Null, Bool, Num, Str, Obj, Arr } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<std::string> keys;  // Obj: parallel to vals
    std::vector<Json> vals;         // Obj values or Arr items

    const Json* get(const std::string& key) const;
    std::string str(const std::string& key, const std::string& def = "") const;
    double num(const std::string& key, double def = 0) const;
};
bool json_parse(const std::string& text, Json* out);
std::string json_escape(const std::string& s);   // no surrounding quotes

/* ---- settings (hub.json) ---- */
struct Settings {
    std::string name;                      // empty = use the Windows user name (caller fills in)
    std::vector<std::string> rom_folders;  // migrated from the old single "rom_folder"
    std::string server;        // filled from hubnet::default_server() by the caller when empty
    int input_delay = 2;
    std::string last_game;
    std::string smash_exe;     // user-chosen BattleShip.exe
    std::string gb_core = "sameboy";   // default GB/GBC core: "sameboy" | "gambatte"
    bool fullscreen = false;           // start games fullscreen where the runner supports it
    std::string sync_folder;           // save sync folder ("" = off)
    bool sync_auto = true;             // sync after playing / on hub start
    bool sync_states = false;          // include save states in sync + export
    std::string vault_dir;             // "" = default (userdata/saves)
};
std::string settings_to_json(const Settings& s);
bool settings_from_json(const std::string& text, Settings* out);   // missing keys keep defaults in *out
bool load_settings(const std::string& path, Settings* out);
bool save_settings(const std::string& path, const Settings& s);

/* ---- lobby rules ---- */
struct PeerInfo {
    int slot = 0;
    std::string name;
    bool hello = false;                         // hello message received
    std::map<std::string, std::string> games;   // catalog id -> md5 ("native" for native ports)
    std::vector<std::string> keys;              // library game keys the peer owns (hello v2)
};
struct Selectability { bool ok = false; std::string reason; };

/* A game is selectable only if every member owns it. `game_id` is either an N64 catalog id (then
 * everyone must have the SAME md5; host's is the reference) or a library game key (everyone must
 * list that key). */
Selectability game_selectable(const std::string& game_id, int max_players,
                              const std::vector<PeerInfo>& peers, int host_slot);

/* hello v2: {"kind":"hello","version":"2","games":{catalogId:md5,...},"keys":["n64:..","gba:.."]}.
 * "games" is kept for old clients (N64 catalog md5s); "keys" advertises every game key. */
std::string hello_json(const std::string& version, const std::map<std::string, std::string>& games,
                       const std::vector<std::string>& keys = {});
/* Same content, split into several messages (each <= ~max_bytes) because the lobby server drops
 * text frames above 4096 bytes. Part 0 carries "games"; later parts ("part":n) append keys. */
std::vector<std::string> hello_messages(const std::string& version, const std::map<std::string, std::string>& games,
                                        const std::vector<std::string>& keys, size_t max_bytes = 3000);
bool parse_hello(const Json& body, PeerInfo* out);
/* `core` (optional): the host's GB/GBC core so all peers run the same one. */
std::string launch_json(const std::string& game, int launch, int delay, unsigned seed,
                        const std::string& core = "");

/* ---- library list helpers ---- */
struct ListItem {
    std::string key;
    std::string title;
    std::string system;     // system short code ("n64","gba",...)
    bool pinned = false;    // sorts before everything else (the native Smash entry)
};
/* Indices of items that match `system_filter` ("" = all) and contain `query` (case-insensitive
 * substring of the title), pinned first, then sorted by title (case-insensitive), then key. */
std::vector<int> filter_sort(const std::vector<ListItem>& items, const std::string& system_filter,
                             const std::string& query);
/* Indices of the `n` entries with the largest non-zero last_played, newest first. */
std::vector<int> pick_recent(const std::vector<unsigned long long>& last_played, int n);
/* "Never played" / "Under a minute" / "35 min" / "2h 05m". */
std::string format_play_time(unsigned long long seconds);
/* Windows user name -> usable player name (printable, max 16 chars, "Player" if empty). */
std::string sanitize_player_name(const std::string& s);

/* ---- INI helpers (Dolphin config) ---- */
/* Returns text with [section] key = value set (section/key matched case-insensitively; added if missing). */
std::string ini_set(const std::string& text, const std::string& section, const std::string& key,
                    const std::string& value);
/* Value of key in section, or def. */
std::string ini_get(const std::string& text, const std::string& section, const std::string& key,
                    const std::string& def = "");

/* ---- save vault listing (meta.json) ---- */
struct VaultMeta {
    std::string key, title, system, device;
    unsigned long long updated = 0;
};
bool parse_vault_meta(const std::string& json, VaultMeta* out);

/* "0,1,2,-1": controller port i is owned by the i-th member (sorted by slot), -1 if none. */
std::string ports_arg(std::vector<int> member_slots);

/* Human text for an n64run exit code. */
std::string exit_code_text(unsigned long code);

/* Uppercase, strip blanks, cap at 6 chars. */
std::string normalize_code(const std::string& s);

} // namespace hubui
