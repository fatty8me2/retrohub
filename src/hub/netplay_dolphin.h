#pragma once
/* GameCube online play through Dolphin's own NetPlay.
 * Dolphin 2509 has no command-line or config way to host/join/start a session, so RetroHub pre-writes the
 * [NetPlay] config and drives Dolphin's own NetPlay dialogs with Windows UI Automation (PowerShell driver,
 * no pixels, no focus needed). The launch spec rides in the host's lobby "launch" message under "np_gc". */

#include <cstdint>
#include <string>

namespace gcnp {

enum class Match { Same, DifferentDisc, Unknown };

struct Spec {                 // the "np_gc" object
    std::string mode = "traversal";   // "traversal" (host code via Dolphin's traversal server) or "direct" (addr:port)
    std::string host_code;            // traversal: 8 hex chars; empty until the host's Dolphin reports it
    std::string addr;                 // direct
    int port = 2626;                  // direct
    std::string game_id;              // "GM4E01"
    std::string title;                // Dolphin's list title is matched by "(GAMEID)"
    std::string sha256;               // of the host's disc file ("" = unknown)
    uint64_t size = 0;                // bytes of the host's disc file
    int players = 2;                  // expected NetPlay players (room size)
    int disc = -1;                    // disc number (header 0x06), -1 = unknown
    int rev = -1;                     // disc revision (header 0x07), -1 = unknown
};

/* What Dolphin NetPlay itself compares: game ID + disc number + revision from the disc header. The container (iso/ciso/rvz/wia/gcz) and the
 * file size/hash do not matter. Read from iso/gcm/ciso/rvz/wia; gcz (compressed header) and unreadable files are "unknown" and never block. */
struct DiscVer { bool ok = false; std::string id; int disc = -1; int rev = -1; };
DiscVer disc_version(const std::string& path);
DiscVer disc_version_from_header(const unsigned char* h, size_t n);   // 0x80+ bytes of the disc header
Match compare_version(const Spec& host, const DiscVer& mine);
/* "<theirs> vs <mine>" wording for a blocked guest; "" when compatible. */
std::string version_diff(const Spec& host, const DiscVer& mine);

std::string spec_json(const Spec& s);                       // {"mode":...}  (no outer kind)
bool spec_parse(const std::string& json, Spec* out);        // accepts the object text; false if game_id/mode invalid
/* Insert `"np_gc":{...}` into an existing launch message (before its closing brace). */
std::string add_to_launch(const std::string& launch_json, const Spec& s);
/* Extract the np_gc object text from a launch message ("" when absent). */
std::string extract_from_launch(const std::string& launch_json);
/* ",\"np_gc\":{...}" (leading comma) for hubui::launch_json's extra_json parameter. */
std::string launch_fragment(const Spec& s);

/* Identity check of this PC's disc file against the host's. Unknown when the host sent no hash. */
Match compare_disc(const Spec& host, uint64_t my_size, const std::string& my_sha256);
const char* match_text(Match m);

bool valid_host_code(const std::string& s);                 // 8 lowercase/uppercase hex chars

/* Dolphin.ini rewrite: [NetPlay] Nickname/TraversalChoice/HostCode/Address/ConnectPort/HostPort,
 * [General] ISOPath0 (folder holding the disc, so the NetPlay game list finds it). mute: No Audio Output (tests). */
struct IniParams {
    bool host = false;
    std::string nickname;
    std::string iso_dir;
    bool mute = false;
    int ping_ms = -1;             // worst round trip to another PC (-1 unknown): sizes the pad buffer
    bool relayed = false;         // a link goes through the server relay
    bool write_save = false;      // host only: keep this session's memory-card changes (Dolphin 'Write save data'); off = discarded
};
bool dual_core_allowed(const std::string& game_id);
/* NetPlay pad buffer (SI polls) for the session's worst round trip in ms (-1 unknown). Too small and every Dolphin in the room blocks
 * on each pad poll waiting for the other side's input (the whole room runs slow and stutters); too big only adds input lag.
 * Dolphin's guide: 1 per 15 ms; measured with qa/gc_online_speed.ps1 (docs/DOLPHIN.md "Online speed"). */
int pad_buffer_for_ping(int ping_ms, bool relayed);
/* Hysteresis for a live room: raise at once, lower only when the wanted value is 3 or more below the current one. cur < 0 = none yet. */
int next_pad_buffer(int cur, int want);
std::string ini_patch(const std::string& ini, const Spec& s, const IniParams& p);

/* Host memory card for NetPlay ("Sync Saves"). Dolphin 2509's NetPlay server sends the guests the .gci files of
 * GCIFolderAPath\<REGION> (NetPlayServer.cpp SyncSaveData -> Config::GetGCIFolderPath, which ignores GCIFolderAPathOverride), while
 * the host's own card is the override folder. RetroHub's card is that override, so guests used to get an EMPTY card while the host
 * played on its real save: different boot paths, "Possible desync detected" at the boot card read (Super Mario Strikers frame 420),
 * different menus/unlocks for every game with a save. This wipes <sync_root>, copies the .gci files of card_dir into
 * <sync_root>\USA, \EUR and \JPN (the folder names Dolphin appends for each disc region) and returns the extra Dolphin argument
 * ` -C "Dolphin.Core.GCIFolderAPath=<sync_root>"`. The host still plays and saves on card_dir (the override); only the copy is sent.
 * Returns "" (and *err) when the copy fails. Paths are UTF-8. */
std::string host_card_sync_args(const std::string& card_dir, const std::string& sync_root, std::string* err = nullptr);

/* "Game (GM4E01)" row finder helper: Dolphin lists NetPlay games as "<title> (<id>)". */
std::string list_item_suffix(const Spec& s);

/* Seconds a guest waits before connecting so NetPlay ports follow room-slot order (slot 1 = first guest). */
int guest_connect_delay_s(int slot);

#ifdef _WIN32
struct Run {
    unsigned long dolphin_pid = 0;
    unsigned long driver_pid = 0;
    std::string status_path;      // driver appends lines: "stage:<name>", "code:<hostcode>", "players:<n>", "error:<text>"
    std::string error;
    bool wait_go = false;         // guest: the driver stops at the open Setup dialog and presses Connect only once signal_go() ran (Dolphin boots while the host is still starting)
    std::string go_path;
    std::string buf_path;         // host: pad buffer the driver types into the NetPlay dialog right before Start (set_host_buffer)
};
/* Starts Dolphin (-b not used: the main window must exist) minimized without activation plus the driver.
 * desktop = "" runs on the user's desktop; a name (tests) runs both on a private invisible desktop. */
bool start(bool host, const std::string& dolphin_exe, const std::string& user_dir, const Spec& s,
           int my_slot, const std::string& work_dir, const std::string& desktop, const std::string& extra_args, Run* out);
/* Last "code:" value written by the host driver ("" if none yet). */
std::string read_code(const Run& r);
std::string read_status(const Run& r);   // whole status file
void set_host_buffer(const Run& r, int frames);   // host: the session's ping is known only once everyone connected
void signal_go(const Run& r);            // guest: the host's NetPlay server is listening, connect now
#endif

} // namespace gcnp
