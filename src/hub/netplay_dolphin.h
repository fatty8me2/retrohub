#pragma once
/* GameCube online play through Dolphin's own NetPlay.
 * Dolphin 2509 has no command-line or config way to host/join/start a session, so RetroHub pre-writes the
 * [NetPlay] config and drives Dolphin's own NetPlay dialogs with Windows UI Automation (PowerShell driver,
 * no pixels, no focus needed). The launch spec rides in the host's lobby "launch" message under "np_gc". */

#include <cstdint>
#include <string>

namespace gcnp {

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
};

std::string spec_json(const Spec& s);                       // {"mode":...}  (no outer kind)
bool spec_parse(const std::string& json, Spec* out);        // accepts the object text; false if game_id/mode invalid
/* Insert `"np_gc":{...}` into an existing launch message (before its closing brace). */
std::string add_to_launch(const std::string& launch_json, const Spec& s);
/* Extract the np_gc object text from a launch message ("" when absent). */
std::string extract_from_launch(const std::string& launch_json);
/* ",\"np_gc\":{...}" (leading comma) for hubui::launch_json's extra_json parameter. */
std::string launch_fragment(const Spec& s);

enum class Match { Same, DifferentDisc, Unknown };
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
    bool write_save = false;      // host only: keep this session's memory-card changes (Dolphin 'Write save data'); off = discarded
};
std::string ini_patch(const std::string& ini, const Spec& s, const IniParams& p);

/* "Game (GM4E01)" row finder helper: Dolphin lists NetPlay games as "<title> (<id>)". */
std::string list_item_suffix(const Spec& s);

/* Seconds a guest waits before connecting so NetPlay ports follow room-slot order (slot 1 = first guest). */
int guest_connect_delay_s(int slot);

/* The PowerShell UIA driver text (Role: host|guest). */
const char* driver_script();

#ifdef _WIN32
struct Run {
    unsigned long dolphin_pid = 0;
    unsigned long driver_pid = 0;
    std::string status_path;      // driver appends lines: "stage:<name>", "code:<hostcode>", "players:<n>", "error:<text>"
    std::string error;
    bool wait_go = false;         // guest: the driver stops at the open Setup dialog and presses Connect only once signal_go() ran (Dolphin boots while the host is still starting)
    std::string go_path;
};
/* Starts Dolphin (-b not used: the main window must exist) minimized without activation plus the driver.
 * desktop = "" runs on the user's desktop; a name (tests) runs both on a private invisible desktop. */
bool start(bool host, const std::string& dolphin_exe, const std::string& user_dir, const Spec& s,
           int my_slot, const std::string& work_dir, const std::string& desktop, const std::string& extra_args, Run* out);
/* Last "code:" value written by the host driver ("" if none yet). */
std::string read_code(const Run& r);
std::string read_status(const Run& r);   // whole status file
void signal_go(const Run& r);            // guest: the host's NetPlay server is listening, connect now
#endif

} // namespace gcnp
