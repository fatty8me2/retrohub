#pragma once
/* Pure (UI/OS independent) logic for RetroHub: tiny JSON, settings, lobby rules. */

#include <map>
#include <string>
#include <utility>
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
    int input_delay = 2;               // manual delay 1-8 frames (used when delay_auto is off)
    bool delay_auto = true;            // "Auto (recommended)": runners measure the connection
    std::string last_game;
    std::string smash_exe;     // user-chosen BattleShip.exe
    std::string gb_core = "sameboy";   // default GB/GBC core: "sameboy" | "gambatte"
    bool fullscreen = false;           // start games fullscreen where the runner supports it
    std::string sync_folder;           // save sync folder ("" = off)
    bool sync_auto = true;             // sync after playing / on hub start
    bool sync_states = false;          // include save states in sync + export
    bool sync_card_dismissed = false;  // "Keep your saves on all your PCs" card on Home was dismissed ("Not now")
    bool smash_save_notice_shown = false;  // the "Using your synced Smash save" toast has been shown once
    std::string vault_dir;             // "" = default (userdata/saves)
    bool download_art = true;          // fetch cover art from the libretro-thumbnails project
    int sidebar_mode = 0;              // 0 auto (collapses when narrow), 1 always expanded, 2 always icons-only
    bool show_friends = true;          // friends panel open
    bool appear_offline = false;       // friends see me as offline
    bool notif_sound = true;           // play a sound for invites / messages (setting only for now)
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
    std::string app;                            // RetroHub release version from hello ("" = old build, unknown)
};
struct Selectability { bool ok = false; std::string reason; };

/* A game is selectable only if every member owns it. `game_id` is either an N64 catalog id (then
 * everyone must have the SAME md5; host's is the reference) or a library game key (everyone must
 * list that key). */
Selectability game_selectable(const std::string& game_id, int max_players,
                              const std::vector<PeerInfo>& peers, int host_slot);

/* Shown when a room has 3-4 members and Smash Classic (smash64) is selected: its online play is 2 players.
 * Names members (judged from their hello) who lack the emulated ssb64rom. Empty when the notice does not apply. */
std::string smash_room_notice(const std::vector<PeerInfo>& peers);

/* hello v2: {"kind":"hello","version":"2","games":{catalogId:md5,...},"keys":["n64:..","gba:.."]}.
 * "games" is kept for old clients (N64 catalog md5s); "keys" advertises every game key. */
std::string hello_json(const std::string& version, const std::map<std::string, std::string>& games,
                       const std::vector<std::string>& keys = {}, const std::string& app = "");
/* Same content, split into several messages (each <= ~max_bytes) because the lobby server drops
 * text frames above 4096 bytes. Part 0 carries "games"; later parts ("part":n) append keys. */
std::vector<std::string> hello_messages(const std::string& version, const std::map<std::string, std::string>& games,
                                        const std::vector<std::string>& keys, size_t max_bytes = 3000,
                                        const std::string& app = "");
bool parse_hello(const Json& body, PeerInfo* out);

/* ---- app version gate (everyone in a room must run the same RetroHub release) ---- */
/* Dotted numeric compare, tolerant of a leading "v" and trailing suffixes ("1.10.0-beta"): -1/0/1.
 * Returns 2 when either side has no parsable number. */
int compare_app_versions(const std::string& a, const std::string& b);
/* "v1.10.0" form ("" -> ""). */
std::string app_version_label(const std::string& v);
struct VersionGate { bool ok = true; std::string reason; };
/* Host-side: blocks Launch while any member that sent a hello runs a different (or unknown) version
 * than `host_app`. `allow_mismatch` (dev override) always passes. */
VersionGate version_gate(const std::string& host_app, const std::vector<PeerInfo>& peers, bool allow_mismatch = false);
enum class GuestVersionAction { None, Waiting, UpdateNow, HostNeedsUpdate };
/* Guest-side: host hello not received yet -> Waiting; older than the host -> UpdateNow; newer, or the host's
 * hello arrived without an app field (old build) -> HostNeedsUpdate. */
GuestVersionAction guest_version_action(const std::string& my_app, const std::string& host_app, bool host_hello);
/* `core` (optional): the host's GB/GBC core so all peers run the same one. */
/* `delay` 0 = auto. `share_host_save`: N64 online, every PC uses the host's save. */
struct PortPlan;
std::string launch_json(const std::string& game, int launch, int delay, unsigned seed,
                        const std::string& core = "", bool share_host_save = false, const PortPlan* plan = nullptr);
/* " --delay auto" / " --delay N" (leading space). */
std::string delay_arg(int delay);

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

/* ---- home screen composition (pure; the UI only draws the result) ---- */
struct ShelfGame {
    std::string key, title, system;     // system short code
    bool favorite = false;
    unsigned long long last_played = 0;
    bool online = false;                // catalog says it is fun online with friends
    bool pinned = false;
    std::string genre;                  // catalog genre ("" / unknown = "Other")
};
struct Shelf {
    std::string id;      // "recent" "favs" or "genre:<Genre>" ("genre:Other:<sys>" for uncatalogued games)
    std::string title;   // "Continue Playing" ...
    std::vector<int> items;   // indices into the ShelfGame vector
};
/* Genre shelves in display order. Games whose genre is not in this list go to "Other". */
const std::vector<std::string>& genre_order();     // Party Fighting Racing Shooter Platformer Adventure RPG Strategy Sports Puzzle Action
/* Genre a game is filed under: its own when known, else "Other". */
std::string effective_genre(const std::string& genre);
/* Shelves in display order: recent (<= max_recent), favorites, then one shelf per genre that has games
 * (genre_order(), then "Other"). Every game lands in exactly ONE genre shelf; "Other" is split per system
 * (n64, gc, gba, gbc, gb). Empty shelves are omitted. Within a shelf: recent = newest first; the others
 * pinned first then title (case-insensitive). */
std::vector<Shelf> compose_shelves(const std::vector<ShelfGame>& games, int max_recent = 12);
std::string system_shelf_title(const std::string& system_short);   // "Nintendo 64", ...
/* Catalog entries to advertise ("Top picks you don't have yet"): indices where !owned[i], games
 * flagged online first, otherwise catalog order, at most max_n. */
std::vector<int> pick_unowned(const std::vector<bool>& owned, const std::vector<bool>& online, int max_n);
/* Which game the hero header shows by default: preferred key if present, else most recently
 * played, else first favorite, else first. -1 when empty. */
int pick_hero(const std::vector<ShelfGame>& games, const std::string& preferred_key);

/* ---- horizontal shelf scrolling (pure so it can be tested) ---- */
struct FRect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
/* Index of the first rect containing (x,y), or -1. */
int rect_hit(const std::vector<FRect>& rects, float x, float y);
/* Pixels to add to a row's scroll target for a wheel event over it: the vertical wheel (up = left) and the
 * horizontal wheel (right = right) both scroll the row, ~1.5 tiles per notch. */
float shelf_wheel_delta(float wheel_x, float wheel_y, float tile_w, float gap);
/* Has the pointer moved far enough from the press point for a click to become a drag-pan? */
bool drag_exceeds(float dx, float dy, float threshold);
/* Distance of one "page" arrow click: whole tiles that fit in the row (at least one). */
float shelf_page_step(float view_w, float tile_w, float gap, float bleed);
/* New scroll target that makes [left,right] (content coords) visible with `margin`, clamped to [0,max_scroll]. */
float shelf_reveal(float left, float right, float target, float view_w, float margin, float max_scroll);
float shelf_clamp(float v, float max_scroll);
/* Max scroll of a row of `count` tiles: content (with `bleed` on both ends) minus the view; never negative. */
float shelf_max_scroll(int count, float tile_w, float gap, float bleed, float view_w);

/* ---- layout / animation math ---- */
struct GridLayout { int cols = 1; float tile_w = 0; };
/* Columns of tiles in `avail` px: as many as fit at >= min_w, tiles grow up to max_w. */
GridLayout grid_layout(float avail, float min_w, float max_w, float gap);
/* Tile width for a horizontal shelf: whole tiles plus a ~40% peek of the next one. */
float shelf_tile_w(float avail, float min_w, float max_w, float gap);
/* Frame-rate independent exponential approach: v moves toward target at `rate` (1/s). */
float approach(float v, float target, float dt, float rate);
/* How to place box art of size art_w x art_h inside a box_w x box_h tile.
 * cover: art whose aspect is within [cover_lo, cover_hi] of the tile's is cropped to fill it;
 * otherwise it is letterboxed (centered, fully visible). x,y,w,h are relative to the box origin. */
struct ArtFit { bool cover = true; float x = 0, y = 0, w = 0, h = 0, u0 = 0, v0 = 0, u1 = 1, v1 = 1; };
ArtFit fit_art(float art_w, float art_h, float box_w, float box_h, float cover_lo = 0.74f, float cover_hi = 1.32f);
/* Sidebar collapsed? mode 0 = auto (narrower than `narrow_below` px), 1 = never, 2 = always. */
bool sidebar_collapsed(int mode, float window_w, float narrow_below);

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

/* A room member and how many local controllers they play with (1-2). */
struct MemberPads { int slot = 0; int locals = 1; };
struct PortPlan {
    std::string ports;            // --ports value, e.g. "0,0,1,-1" (repeated slot ids = several pads on one PC)
    int players = 0;              // --players: number of members
    std::vector<int> locals;      // effective local count per member, in slot order (after the 4-port cap)
};
/* Members sorted by slot; each gets `locals` (clamped 1-2) consecutive ports; total capped at 4 (every
 * member keeps one port first, extra pads are granted in slot order while ports remain). */
PortPlan build_ports(std::vector<MemberPads> members);
/* The HOST builds the plan once and ships it in the launch message ("ports":[4 owner slots or -1],
 * "players":n) so every PC passes identical --ports/--players. Guests call this: true + the --ports string and
 * player count when the message carries a well-formed plan (4 entries in -1..3, players == number of distinct
 * owners, owners packed first); false when absent or malformed (older host: derive locally). */
bool parse_launch_plan(const Json& launch_msg, std::string* ports, int* players);

/* Save sync must never run while a game process is alive: requests made then are queued. */
enum class SyncAction { Start, Queue };
struct SyncGate { bool pending = false; bool manual = false; };
SyncAction sync_request(SyncGate& g, bool manual, bool game_running, bool job_running);
/* Call every frame: true (and clears the queue) when a queued sync may start now. */
bool sync_take_pending(SyncGate& g, bool game_running, bool job_running, bool* manual);

/* Human text for an n64run exit code. */
std::string exit_code_text(unsigned long code);

/* Uppercase, strip blanks, cap at 6 chars. */
std::string normalize_code(const std::string& s);

/* ---- friends (pure helpers; the UI maps hubnet::Friend onto FriendRow) ---- */
enum FriendState { FS_Offline = 0, FS_Online = 1, FS_InLobby = 2, FS_InGame = 3 };
struct FriendRow {
    std::string uid, name, game_title;
    int state = FS_Offline;
    bool joinable = false;
    int players = 0, max_players = 0;
    long long updated_ms = 0;      // last presence update (offline: roughly "last seen")
};
struct FriendGroups { std::vector<int> online, offline; };
/* Online first (joinable lobby, in game, in lobby, online; then name, case-insensitive), then offline (most
 * recently seen first, then name). Returns indices into `rows`. */
FriendGroups group_friends(const std::vector<FriendRow>& rows);
/* "Playing Mario Kart 64" / "In lobby . Mario Party 2 . 2/4" / "Online" / "Last seen 3h ago" / "Offline". */
std::string friend_status_line(const FriendRow& r, long long now_ms);
/* "just now" / "5 min ago" / "3h ago" / "2d ago" */
std::string format_ago_ms(long long now_ms, long long then_ms);
/* "RH-7KQ2-M9XD" from "7kq2m9xd", "RH-7KQ2-M9XD", " rh7kq2 m9xd " ...; "" when not a valid code (8 letters/digits). */
std::string normalize_friend_code(const std::string& s);
inline bool invite_expired(long long expires_at_ms, long long now_ms) { return expires_at_ms > 0 && now_ms >= expires_at_ms; }
/* Seconds left before the invite expires (0 when expired or unknown). */
int invite_seconds_left(long long expires_at_ms, long long now_ms);
/* A room can be joined while it has a free slot and the game has not been launched. */
inline bool lobby_joinable(int players, int max_players, bool launched) { return !launched && players > 0 && players < max_players; }
/* A friend row offers "Join": in a lobby, or in a game launched from a room, with a free slot (and a known code). */
inline bool friend_can_join(const FriendRow& r) { return (r.state == FS_InLobby || r.state == FS_InGame) && r.joinable; }
/* Toast shown when a join is clicked while our own game is running (performed when the game closes). */
std::string queued_join_text(const std::string& friend_name);

/* ---- Smash Classic (native BattleShip.exe) launch plumbing ---- */
/* Vault identity of the native port's save: system "n64", game key "native:smash64" (stable; shared by every PC). */
inline const char* kSmashSaveSystem = "n64";
inline const char* kSmashSaveKey = "native:smash64";
/* Full file path BattleShip must use (env SSB64_SAVE_PATH is a FILE path): <battery_dir>\ssb64_save.bin. */
std::string smash_save_file(const std::string& battery_dir);
/* One-time migration: copy <BattleShip dir>\ssb64_save.bin into the vault only when the vault has none yet. */
inline bool smash_should_migrate_save(bool vault_file_exists, bool legacy_file_exists) { return !vault_file_exists && legacy_file_exists; }
/* Both saves exist and their bytes differ: the vault copy wins, the legacy file is left alone (shown once). */
inline bool smash_save_conflict(bool vault_file_exists, bool legacy_file_exists, const std::string& vault_bytes, const std::string& legacy_bytes) {
    return vault_file_exists && legacy_file_exists && vault_bytes != legacy_bytes;
}
/* Toast text for that conflict; `legacy_path` is the old <BattleShip dir>\ssb64_save.bin. */
std::string smash_save_conflict_text(const std::string& legacy_path);
struct RomRef { std::string catalog_id, serial, path; };
/* The Smash 64 ROM in the scanned library: catalog id "ssb64rom" or serial NALE (USA), non-empty path. "" when absent. */
std::string find_smash_rom(const std::vector<RomRef>& lib);
/* Environment for every Smash launch: SSB64_BASEROM (when a ROM is known) and SSB64_SAVE_PATH. */
std::vector<std::pair<std::string, std::string>> smash_launch_env(const std::string& rom_path, const std::string& save_file);

} // namespace hubui
