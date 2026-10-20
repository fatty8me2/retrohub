#pragma once
/* Pure (UI/OS independent) logic for ClassicHub: tiny JSON, settings, lobby rules. */

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
    std::string name = "Player";
    std::string rom_folder;
    std::string server;        // filled from hubnet::default_server() by the caller when empty
    int input_delay = 2;
    std::string last_game;
    std::string smash_exe;     // user-chosen BattleShip.exe
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
    std::map<std::string, std::string> games;   // gameId -> md5 ("native" for native ports)
};
struct Selectability { bool ok = false; std::string reason; };

/* A game is selectable only if every member owns it with the SAME md5 (host's md5 is the reference). */
Selectability game_selectable(const std::string& game_id, int max_players,
                              const std::vector<PeerInfo>& peers, int host_slot);

std::string hello_json(const std::string& version, const std::map<std::string, std::string>& games);
bool parse_hello(const Json& body, PeerInfo* out);
std::string launch_json(const std::string& game, int launch, int delay, unsigned seed);

/* "0,1,2,-1": controller port i is owned by the i-th member (sorted by slot), -1 if none. */
std::string ports_arg(std::vector<int> member_slots);

/* Human text for an n64run exit code. */
std::string exit_code_text(unsigned long code);

/* Uppercase, strip blanks, cap at 6 chars. */
std::string normalize_code(const std::string& s);

} // namespace hubui
