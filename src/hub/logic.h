#pragma once
/* Pure (UI/OS independent) logic for RetroHub: tiny JSON, settings, lobby rules. */

#include "json.h"
#include "state_slots.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hubui {

/* ---- JSON: the shared reader/writer (src/common/json.h); json::parse / json::escape / json::Value ---- */
using Json = json::Value;

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
    bool boot_anim = true;             // "Play the start animation" (boot_anim.h)
    bool boot_sounds = true;           // "Startup sounds"
    int boot_volume = 70;              // startup sound volume 0-100
    bool boot_short = false;           // "Short version" (~1 s)
    bool boot_skip_online = true;      // "Skip in online games"
    std::string real_boot_off;         // "Real console startup" turned OFF for these systems (comma separated short names); default on where the file exists
    bool start_hint = true;            // "Show controller tips when a game starts": ~5 s card (View+Y menu, View+Menu leave)
    std::string sync_folder;           // save sync folder ("" = off)
    bool sync_auto = true;             // sync after playing / on hub start
    bool backup_shots = false;         // "Back up screenshots": screenshots count toward the cloud large-item quota (default off)
    std::string library_sort = "title"; // SortMode key: title | last_played | most_played | recently_added
    bool cloud_states = true;          // "Back up save states to the cloud" (within the smart cap)
    bool cloud_enabled = true;         // cloud saves v2: automatic through the RetroHub account; false = "Pause cloud sync"
    bool sync_card_dismissed = false;  // "Keep your saves on all your PCs" card on Home was dismissed ("Not now")
    bool smash_save_notice_shown = false;  // the "Using your synced Smash save" toast has been shown once
    std::string vault_dir;             // "" = default (userdata/saves)
    int rom_cache_limit_gb = 40;       // unpacked-games cache limit in GB (0 = unlimited)
    bool download_art = true;         // fetch cover art from the libretro-thumbnails project
    int sidebar_mode = 0;              // 0 auto (collapses when narrow), 1 always expanded, 2 always icons-only
    bool show_friends = true;          // friends panel open
    bool notify_in_game = true;        // Windows toast for invites while a game is running
    bool notif_sound = true;           // play a sound for invites / messages (setting only for now)
    bool share_games = true;           // "Show my games to friends": publish the list of game titles (never files) so friends see them greyed out
    bool show_friend_games = true;     // library chip "Show friends' games"
    bool share_playtime = true;        // "Share my play time with friends": per-game totals (whole minutes) go to the friends server when a session ends
    bool auto_friend = true;           // "Automatically add people I play with as friends" (sent to the server in the hello)
    bool username_claimed = false;     // this PC has a username the server confirmed (name below IS that username); false = still the provisional Windows name
    bool find_games_done = false;      // the first-run "Find my games" step was finished or skipped
    bool setup_done = false;           // the first-run setup wizard was finished or skipped
    std::string changelog_seen;        // version whose "What's new" popup was shown/skipped ("" = never)
    bool kt_intro_seen = false;        // Kanto Together: the "first session" guidance was shown once
    std::string games_dest;            // Add games: chosen destination folder ("" = the RetroHub Games folder)
    std::string ps1_bios;              // user-chosen PlayStation BIOS file ("" = look next to the games, in RetroArch's system folder, ...)
    bool ps1_openbios = false;         // play PlayStation games with the bundled OpenBIOS when no real BIOS is found
    std::string cemu_exe;              // user-chosen Cemu.exe ("" = find an existing install, else download one)
    bool cemu_manage_controls = true;  // "Let RetroHub manage Wii U controls" for the user's own Cemu (backup made first)
    int settings_tab = 0;             // last Settings tab (0 General .. 5 Account), remembered
    std::string ps2_bios_dir;         // user-chosen folder with the PS2 BIOS ("" = look next to the games / Desktop\Everything\BIOS)
};
std::string settings_to_json(const Settings& s);
bool settings_from_json(const std::string& text, Settings* out);   // missing keys keep defaults in *out
bool load_settings(const std::string& path, Settings* out);
bool save_settings(const std::string& path, const Settings& s);
/* RetroHub-Setup.exe drops userdata/installer_games_dir.txt with the games folder the player chose.
 * Adds that folder to s->rom_folders (never removes or reorders existing ones). Returns true when the
 * list changed. Tolerates a UTF-8 BOM, CRLF, surrounding quotes/spaces; rejects empty/relative paths;
 * duplicates are detected ignoring case and trailing slashes. The caller deletes the file afterwards. */
bool merge_installer_games_dir(Settings* s, const std::string& file_text);

/* ---- lobby rules ---- */
struct PeerInfo {
    int slot = 0;
    std::string name;
    bool hello = false;                         // hello message received
    std::map<std::string, std::string> games;   // catalog id -> md5 ("native" for native ports)
    std::vector<std::string> keys;              // library game keys the peer owns (hello v2)
    std::string app;                            // RetroHub release version from hello ("" = old build, unknown)
    int parts_expected = 1, parts_got = 1;      // hello is split into several messages when the key list is long
    bool hello_complete() const { return hello && parts_got >= parts_expected; }
};
struct Selectability { bool ok = false; std::string reason; };

/* A game is selectable only if every member owns it. `game_id` is either an N64 catalog id (then
 * everyone must have the SAME md5; host's is the reference) or a library game key (everyone must
 * list that key). */
Selectability game_selectable(const std::string& game_id, int max_players,
                              const std::vector<PeerInfo>& peers, int host_slot, bool remote_play = false);
/* remote_play: only the host needs the game (guests get a video stream), so only the host's ownership is checked. */

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
/* Guest-side: why the "I'm ready" control must be disabled ("" = may ready up). Same inputs as guest_version_action;
 * `allow_mismatch` is the dev override. The UI shows the returned text next to the disabled control. */
std::string ready_block_reason(const std::string& my_app, const std::string& host_app, bool host_hello, bool allow_mismatch = false);
/* Guest-side: host hello not received yet -> Waiting; older than the host -> UpdateNow; newer, or the host's
 * hello arrived without an app field (old build) -> HostNeedsUpdate. */
GuestVersionAction guest_version_action(const std::string& my_app, const std::string& host_app, bool host_hello);

/* ---- room action banner (READY / LAUNCH at the top of the room) ---- */
struct RoomMember { std::string name; bool ready = false; bool host = false; bool me = false; };
enum class RoomTone { Neutral, Good, Warn, Bad };
struct RoomActionState {
    std::string headline, detail, button_label;
    RoomTone tone = RoomTone::Neutral;
    bool is_launch = false;        // host: the button is Launch; guest: the button toggles Ready
    bool button_enabled = false;
    bool ready_on = false;         // guest: currently ready
    int ready_count = 0, total = 0;
};
/* "Mia", "Mia and Zed", "Mia, Zed and Kai". */
std::string join_names(const std::vector<std::string>& names);
/* `block` = reason the chosen game cannot start for this PC/room right now ("" = none; computed by the page from
 * missing game / version / unpacking / Kanto / in-game checks). The host counts as ready. */
RoomActionState room_action_state(bool host, bool have_game, const std::string& game_title, const std::string& block,
                                  bool my_ready, const std::vector<RoomMember>& members);
/* `core` (optional): the host's GB/GBC core so all peers run the same one. */
/* `delay` 0 = auto. `share_host_save`: N64 online, every PC uses the host's save. */
struct PortPlan;
struct MatchSpec;
std::string launch_json(const std::string& game, int launch, int delay, unsigned seed,
                        const std::string& core = "", bool share_host_save = false, const PortPlan* plan = nullptr,
                        const MatchSpec* match = nullptr, const std::string& extra_json = "");   // extra_json: ",\"np_x\":{...}" fragment
/* The `np_ds` launch-spec value for Nintendo DS games ({"v":1,"mode":"wireless"}): every PC runs its own emulator and
 * only the DS local-wireless frames travel through the room (retrorun --ds-link). Pass ",\"np_ds\":" + np_ds_json() as extra_json. */
std::string np_ds_json();
/* Guest side: true when the launch message carries a DS wireless spec this build understands (absent = older host,
 * which is still wireless for any DS game the catalog marks online). */
bool parse_np_ds(const Json& launch_msg, std::string* mode);
/* " --delay auto" / " --delay N" (leading space). */
std::string delay_arg(int delay);

/* ---- library list helpers ---- */
struct ListItem {
    std::string key;
    std::string title;
    std::string system;     // system short code ("n64","gba",...)
    bool pinned = false;    // sorts before everything else (the native Smash entry)
    unsigned long long last_played = 0, play_seconds = 0, added = 0;   // unix seconds / seconds / file time (sort modes)
    bool ghost = false;     // a friend's game you do not own: sorts after every owned game of its console
};
/* Strict-weak order for the library: pinned first, then by `mode` (Title A-Z; Last played / Most played / Recently added newest or
 * biggest first, never-played last), ties broken by title then key. */
bool list_item_less(const ListItem& a, const ListItem& b, SortMode mode);
/* Indices of items that match `system_filter` ("" = all) and contain `query` (case-insensitive
 * substring of the title), pinned first, then sorted by title (case-insensitive), then key. */
std::vector<int> filter_sort(const std::vector<ListItem>& items, const std::string& system_filter,
                             const std::string& query, SortMode mode = SortMode::Title);
/* Indices of the `n` entries with the largest non-zero last_played, newest first. */
std::vector<int> pick_recent(const std::vector<unsigned long long>& last_played, int n);
/* "Never played" / "Under a minute" / "35 min" / "2h 05m". */
std::string format_play_time(unsigned long long seconds);
/* Friends' play time (coarse): "45m", "3h", "14h 30m" (minutes in, "" for < 1). */
std::string friend_hours_text(int minutes);
/* Game details line: the top `max_n` friends who played `game_key`, "Mickey 14h · camer 3h" ("" if none). `names` maps uid -> display name;
 * friends missing from it, and uids in `hidden` (blocked), are skipped. Ties keep name order. */
struct PlaytimeEntry { std::string game_key, title; int minutes = 0; };
std::string friends_playtime_line(const std::map<std::string, std::vector<PlaytimeEntry>>& by_uid, const std::map<std::string, std::string>& names,
                                  const std::string& game_key, size_t max_n = 3);
/* Friend actions menu: "Top games: Smash 64 14h, Metroid 3h" for one friend ("" if none). */
std::string friend_top_games_text(const std::vector<PlaytimeEntry>& games, size_t max_n = 3);
/* Windows user name -> usable player name (printable, max 16 chars, "Player" if empty). */
std::string sanitize_player_name(const std::string& s);

/* ---- All Games: one section per console ---- */
struct ConsoleSection {
    std::string system;           // system short code, "other" for unknown codes
    std::string title;            // "Nintendo 64" ...
    std::vector<int> items;       // indices into the ListItem vector, pinned first then A-Z
};
/* Groups items[indices] (all of them when `indices` is null) into console sections in console_order(), unknown
 * system codes last under "Other". Empty consoles are skipped. gb and gbc are separate sections. */
std::vector<ConsoleSection> group_by_console(const std::vector<ListItem>& items, const std::vector<int>* indices = nullptr,
                                             SortMode mode = SortMode::Title);

/* ---- home screen composition (pure; the UI only draws the result) ---- */
struct ShelfGame {
    std::string key, title, system;     // system short code
    bool favorite = false;
    unsigned long long last_played = 0;
    bool online = false;                // catalog says it is fun online with friends
    bool pinned = false;
    unsigned long long play_seconds = 0, added = 0;
    bool ghost = false;                 // a friend's game you do not own: never in Continue Playing / Favorites / the hero, sorts last
};
struct Shelf {
    std::string id;      // "recent" "favs" or "console:<system_short>" ("console:other" for unknown systems)
    std::string title;   // "Continue Playing" ...
    std::vector<int> items;   // indices into the ShelfGame vector
};
/* Consoles in display order: n64 gc wiiu 3ds nds gba gbc gb (the native Smash Classic entry is system "n64"). */
const std::vector<std::string>& console_order();
/* Shelves in display order: recent (<= max_recent), favorites, then one shelf per console that has games
 * (console_order(), then "other" for unknown system codes). Every game lands in exactly ONE console shelf.
 * Empty shelves are omitted. Within a shelf: recent = newest first; the others pinned first then title
 * (case-insensitive). */
std::vector<Shelf> compose_shelves(const std::vector<ShelfGame>& games, int max_recent = 12, SortMode mode = SortMode::Title);
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
/* How many more friends the room can take for the selected game (cap = min(4, game cap)); 0 = full. */
int invite_room_left(int members, int game_max);
/* "" when inviting `wanted` more friends fits; else the sentence to show ("The room is full (2/2 for this game)."). */
std::string invite_room_block(int members, int game_max, int wanted);
/* The HOST builds the plan once and ships it in the launch message ("ports":[4 owner slots or -1],
 * "players":n) so every PC passes identical --ports/--players. Guests call this: true + the --ports string and
 * player count when the message carries a well-formed plan (4 entries in -1..3, players == number of distinct
 * owners, owners packed first); false when absent or malformed (older host: derive locally). */
bool parse_launch_plan(const Json& launch_msg, std::string* ports, int* players);


/* ---- Smash match picker (fighter / costume per player, stage + stocks by the host) ----
 * Lobby msg bodies: {"kind":"pick","fighter":f,"costume":c[,"fighter2":f,"costume2":c]}  (every member; fighter -1 = Random;
 * the *2 pair is the 2nd local player) and the host's {"kind":"rules","stage":s,"stocks":n} (stage -1 = Random).
 * The host resolves Randoms with the launch seed, ships the final spec in the launch message ("match") and every PC
 * passes `--match "stage=<id>;stocks=<n>;p0=<fkind>:<costume>;p1=..."` (ids are decimal; port i = i-th used --ports entry).
 * Data comes from common/remix_data.h. Only catalog "remix" uses it today; `remix=false` selects the vanilla roster
 * (12 fighters, 9 stages) so "ssb64rom" can reuse everything by flipping match_game_supported(). */
struct MatchPick { int fighter = -1; int costume = 0; };      // fighter -1 = Random
struct MemberPick { MatchPick p[2]; };                        // [1] = the 2nd local player
struct MatchRules { int stage = -1; int stocks = 3; };        // stage -1 = Random, stocks 1-5
struct MatchSpec { int stage = 0; int stocks = 3; std::vector<MatchPick> players; };   // players[i] = i-th used port
struct FighterChoice { int id = 0; std::string name; bool remix = false; int costumes = 1; };
struct StageChoice { int id = 0; std::string name; bool remix = false; };
bool match_game_supported(const std::string& catalog_id);
std::vector<FighterChoice> fighter_choices(bool remix);       // CSS fighters, Vanilla first
std::vector<StageChoice> stage_choices(bool remix);           // VS-legal stages, Vanilla first
std::string fighter_name(int id);                             // "Random" for -1
std::string stage_name(int id);                               // "Random" for -1
int fighter_costumes(int id);                                 // number of base costumes (>= 1)
bool fighter_valid(int id, bool remix);                       // -1 (Random) is valid
bool stage_valid(int id, bool remix);                         // -1 (Random) is valid
std::string pick_json(const MemberPick& p);
bool parse_pick(const Json& body, MemberPick* out, bool remix = true);   // false when malformed / unknown fighter / bad costume
std::string rules_json(const MatchRules& r);
bool parse_rules(const Json& body, MatchRules* out, bool remix = true);
/* "Falco", "Random", "Falco + Mario" (locals == 2) */
std::string pick_label(const MemberPick& p, int locals);
/* Host: one entry per used port in --ports order (unpicked members count as Random); Randoms resolved from `seed`. */
MatchSpec resolve_match(const std::string& ports, const std::map<int, MemberPick>& picks, const MatchRules& rules,
                        unsigned seed, bool remix = true);
std::string match_arg(const MatchSpec& m);                    // the --match value
std::string match_json(const MatchSpec& m);                   // {"stage":..,"stocks":..,"players":[[f,c],..]}
/* Guest/host: read + validate the launch message's "match" against the port plan. false when absent or invalid. */
bool parse_match(const Json& launch_msg, const std::string& ports, bool remix, MatchSpec* out);

/* Save sync must never run while a game process is alive: requests made then are queued. */
enum class SyncAction { Start, Queue };
struct SyncGate { bool pending = false; bool manual = false; };
SyncAction sync_request(SyncGate& g, bool manual, bool game_running, bool job_running);
/* Call every frame: true (and clears the queue) when a queued sync may start now. */
bool sync_take_pending(SyncGate& g, bool game_running, bool job_running, bool* manual);

/* Human text for an n64run exit code. */
std::string exit_code_text(unsigned long code);
/* Message for a game that ended while a player was missing. `left_name` = who left ("" unknown); only the outcomes a leaver causes
 * (clean 0, connection lost 4, "a player left" 8) are reworded, crashes/desyncs keep their own text. */
std::string player_left_text(const std::string& left_name);
std::string exit_text_for(unsigned long code, const std::string& left_name);

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
    bool in_my_room = false;       // this friend is in the room we are in: shown once, with an "In your room" status
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
inline bool friend_can_join(const FriendRow& r) { return !r.in_my_room && (r.state == FS_InLobby || r.state == FS_InGame) && r.joinable; }
/* Rich presence for rows and the Home shelf: "Smash Classic \xC2\xB7 2/4 \xC2\xB7 12 min" (game, players when known, time in this state from
 * the presence update). Online without a game = "Online"; offline = friend_status_line. */
std::string friend_presence_line(const FriendRow& r, long long now_ms);
/* "12 min" / "1h 5m" / "" (under a minute or unknown). */
std::string format_dur_short(long long ms);
/* "Ask to join": a friend is in a room/game we cannot just join (full, already launched, no known code). Not for people in our room. */
inline bool friend_can_ask(const FriendRow& r) {
    return !r.in_my_room && (r.state == FS_InLobby || r.state == FS_InGame) && !r.joinable;
}
/* Home "Friends are playing" shelf: indices into `rows` of friends in a lobby/game with a known game, joinable first, then the most recent. */
std::vector<int> friends_playing(const std::vector<FriendRow>& rows, size_t max_n = 12);
/* Ask-to-join rate limit, client side (the Worker enforces 1/60 s per target and 10/min overall as well). */
class AskLimiter {
public:
    bool allow(const std::string& uid, long long now_ms) {
        while (!recent_.empty() && now_ms - recent_.front() >= 300000) recent_.erase(recent_.begin());
        auto it = last_.find(uid);
        if (it != last_.end() && now_ms - it->second < 60000) return false;
        if (recent_.size() >= 6) return false;
        last_[uid] = now_ms; recent_.push_back(now_ms);
        return true;
    }
    long long wait_ms(const std::string& uid, long long now_ms) const {
        auto it = last_.find(uid);
        return it == last_.end() ? 0 : std::max(0LL, 60000 - (now_ms - it->second));
    }
private:
    std::map<std::string, long long> last_;
    std::vector<long long> recent_;
};
/* The host receives an ask: ignore a repeat from the same person within 30 s so spam cannot stack toasts. */
inline bool ask_is_repeat(long long last_ms, long long now_ms) { return last_ms > 0 && now_ms - last_ms < 30000; }
std::string ask_toast_title(const std::string& name, const std::string& game);   // "Mia wants to join your game (Smash Classic)"
std::string ask_host_toast_title(const std::string& name, const std::string& game);   // "Mia wants to play Mario Kart 64 - host a room?"
std::string ask_sent_text(const std::string& name);
std::string ask_limited_text(const std::string& name, long long wait_ms);

/* ---- room connection quality (K) ----
 * RTT is measured over the room WebSocket (member to member through the relay server), so it is the relay path: a "great" here means the
 * direct path will be at least as good. `relay` = that member announced relay-only networking. */
enum class PingClass { Unknown, Great, Ok, Poor, Relay };
PingClass ping_class(int rtt_ms, bool relay);
const char* ping_class_label(PingClass c);          // "Great" / "OK" / "Poor" / "Relay" / "..."
std::string ping_text(int rtt_ms, bool relay);       // "42 ms" / "relay" / "-"
/* Smoothed RTT per room slot: the median of the last 5 samples. */
class PingBook {
public:
    void add(int slot, int rtt_ms) { auto& v = s_[slot]; v.push_back(rtt_ms < 0 ? 0 : rtt_ms); if (v.size() > 5) v.erase(v.begin()); }
    int rtt(int slot) const;                        // -1 unknown
    void clear() { s_.clear(); }
    void drop(int slot) { s_.erase(slot); }
private:
    std::map<int, std::vector<int>> s_;
};

/* Toast shown when a join is clicked while our own game is running (performed when the game closes). */
std::string queued_join_text(const std::string& friend_name);
/* A second join click while one is already queued replaces it (only one room can be joined): say so. */
std::string queued_join_replaced_text(const std::string& old_who, const std::string& new_who);
/* Friendly text for a friends-server error. `reason` is the server's reason, `detail` its optional detail
   ("bad-request" + detail "friends-full" etc.). Empty string = nothing worth showing the user. */
std::string friends_error_text(const std::string& reason, const std::string& detail);
/* Toast for an invite_result: successes by name, failures by name. */
std::string invite_result_text(const std::vector<std::string>& ok_names, const std::vector<std::string>& failed_names,
                               const std::string& first_fail_reason, const std::string& code);
/* Join click while our own game runs: queue it, or drop it when we are already in that room. */
enum class JoinWhileRunning { Queue, AlreadyInRoom };
inline JoinWhileRunning join_while_running(bool same_room) { return same_room ? JoinWhileRunning::AlreadyInRoom : JoinWhileRunning::Queue; }
/* A queued join runs once our game has closed. */
inline bool pending_join_ready(bool has_pending, bool game_running) { return has_pending && !game_running; }
/* A retrohub:// / --join request from our own command line (cold start) or from a second launch: held until it can run.
   It cannot run before a player name exists (enter_room refuses without one). If our game is running, fr_join_lobby queues it. */
class LinkJoinQueue {
public:
    void push(const std::string& code) { code_ = code; }          // the latest link wins
    bool pending() const { return !code_.empty(); }
    bool needs_name(bool name_set) const { return pending() && !name_set; }
    bool take(bool name_set, std::string* code) {
        if (!pending() || !name_set) return false;
        if (code) *code = code_;
        code_.clear();
        return true;
    }
private:
    std::string code_;
};
/* After a game closes: play time goes to the vault only for tracked, offline launches of a known system. */
inline bool exit_should_touch_game(bool has_key, bool track, bool online, const std::string& system_short) {
    return has_key && track && !online && !system_short.empty();
}
/* After a game closes: auto sync runs when a sync target exists and auto-sync is on. */
inline bool auto_sync_should_run(bool sync_target, bool sync_auto) { return sync_target && sync_auto; }

/* ---- Smash Classic (native BattleShip.exe) launch plumbing ---- */
/* Vault identity of the native port's save: system "n64", game key "native:smash64" (stable; shared by every PC). */
inline const char* kSmashSaveSystem = "n64";
inline const char* kSmashSaveKey = "native:smash64";
/* Full file path BattleShip must use (env SSB64_SAVE_PATH is a FILE path): <battery_dir>\ssb64_save.bin. */
std::string smash_save_file(const std::string& battery_dir);
/* One-time migration: copy <BattleShip dir>\ssb64_save.bin into the vault only when the vault has none yet. */
inline bool smash_should_migrate_save(bool vault_file_exists, bool legacy_file_exists) { return !vault_file_exists && legacy_file_exists; }
/* Runs the one-time copy (CopyFileW, fail-if-exists) when the rule above says so. Never deletes the legacy file. Returns true when it copied. */
bool smash_migrate_legacy_save(const std::string& legacy_path, const std::string& save_path);
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

/* ROM identity: BattleShip's supported dump is NTSC-U v1.0 (game code NALE), MD5 f7c52568a31aadf26e14dc2b6416b2ed. */
inline const char* kSmashUsaMd5 = "f7c52568a31aadf26e14dc2b6416b2ed";
/* True when `md5_hex` (any case) is the supported USA 1.0 dump. An empty hash (unreadable file) is not supported. */
bool smash_md5_supported(const std::string& md5_hex);
/* What Play does for the native Smash entry. NeedRom: no ROM in the library and no BattleShip.o2r yet, so nothing launches.
 * WarnMd5: the ROM is not the supported dump, so warn, then launch anyway. Launch: go. */
enum class SmashPlay { Launch, WarnMd5, NeedRom };
SmashPlay smash_play_decision(bool o2r_exists, bool rom_found, const std::string& rom_md5_hex);
/* The "Preparing" toast: BattleShip extracts BattleShip.o2r on its first run, so show it while the process runs and the .o2r is still missing. */
inline bool smash_prep_visible(bool proc_running, bool o2r_exists) { return proc_running && !o2r_exists; }
inline const char* smash_prep_text() { return "Preparing Smash Classic (one-time setup)..."; }
inline const char* smash_need_rom_text() { return "Add your Super Smash Bros. (USA) ROM to a game folder, then press Rescan."; }
std::string smash_md5_warn_text(const std::string& md5_hex);

/* ---- lobby host migration ----
 * When the host leaves, the server renumbers the lowest remaining member (`from`) to slot 0 and sends
 * host_changed{from_slot}. Per-slot room state keyed by the old slot moves to the new key; whatever was
 * under `to` (the departed host) is dropped. Other keys are untouched. */
template <class V> void remap_slot_key(std::map<int, V>& m, int from, int to) {
    if (from == to) return;
    m.erase(to);
    auto it = m.find(from);
    if (it == m.end()) return;
    V v = std::move(it->second);
    m.erase(it);
    m[to] = std::move(v);
}
/* Toast shown to the room when the host changes; `me` = the new host is this PC. */
/* Toast for a server-made friendship (joined a room / played together). */
inline std::string auto_friend_text(const std::string& name) { return "You and " + (name.empty() ? std::string("a new player") : name) + " are now friends"; }
/* First message a lobby client sends after the welcome so the room can tell who is sitting where. */
inline std::string lobby_ident_json(const std::string& uid, const std::string& secret) {
    return "{\"type\":\"ident\",\"uid\":\"" + json::escape(uid) + "\",\"secret\":\"" + json::escape(secret) + "\"}";
}
/* The host left: move every per-slot map to the promoted member's slot 0 in one go. `ready` is NOT carried over - the game it
 * belonged to is gone, everyone re-announces (pass the same map the caller then clears). */
template <class... M> void host_left_remap(int from_slot, M&... maps) { (remap_slot_key(maps, from_slot, 0), ...); }
inline std::string host_changed_text(const std::string& name, bool me) { return me ? "You are now the host" : name + " is now the host"; }

} // namespace hubui
