#pragma once
/* Online launch: pure decisions and wording (no UI state, no Windows calls), so a console test can check them.
 *
 *  - launch_busy(): is a launch of this PC's room game already starting? (double-click guard for the host's Launch button)
 *  - host_view_of_guest_fail(): a guest's np_fail text is written for the guest ("RetroHub can't find the game file for X. Open it in
 *    your Library..."). The wire format is unchanged (older RetroHubs show that text as is); the HOST's RetroHub rewrites the known
 *    texts to third person when it shows them, and falls back to the raw text when it does not know the sentence.
 *  - the player-facing wording of launch errors, plus the exact legacy text that still goes over the wire for each (what an older
 *    RetroHub in the same room expects to read). */
#include <string>

namespace hublaunch {

/* ---- double-launch guard ---- */
constexpr unsigned long long kDebounceMs = 5000;

/* Every asynchronous launch state of this PC. GameCube, 3DS, Wii U (remote play) start over several frames, so A.proc alone is not enough. */
struct LaunchStates {
    bool np3_pending = false;     // 3DS: forwarders coming up, Azahar not started yet
    bool np3_active = false;      // 3DS: room link exists
    bool gc_active = false;       // GameCube: a NetPlay session exists (Dolphin starting or running)
    bool gc_guest_wait = false;   // GameCube guest: waiting for the host's Dolphin
    bool gc_forwarder = false;    // GameCube: the hubnet forwarder is up
    bool rp_pending = false;      // Wii U remote play: Cemu started, streamrun not attached yet
    bool rp_stream = false;       // Wii U remote play: streamrun host running
    bool rp_probe = false;        // Wii U remote play: the direct-connection check is running
};
inline bool async_launch_in_progress(const LaunchStates& s) {
    return s.np3_pending || s.np3_active || s.gc_active || s.gc_guest_wait || s.gc_forwarder || s.rp_pending || s.rp_stream || s.rp_probe;
}
/* `pressed_at` = tick of the last launch press (0 = none). Clock wrap-around cannot happen with a 64-bit tick count. */
inline bool within_debounce(unsigned long long now, unsigned long long pressed_at, unsigned long long window = kDebounceMs) {
    return pressed_at != 0 && now >= pressed_at && now - pressed_at < window;
}
inline bool launch_busy(const LaunchStates& s, unsigned long long now, unsigned long long pressed_at) {
    return async_launch_in_progress(s) || within_debounce(now, pressed_at);
}
inline std::string starting_text(const std::string& title) {
    return title.empty() ? std::string("Starting the game...") : "Starting " + title + "...";
}

/* ---- wording shown to the player ---- */
inline std::string core_missing_text(const std::string& system) {
    return "RetroHub is missing a file it needs for " + system + ". Reinstall RetroHub or check for updates.";
}
inline std::string start_failed_text(const std::string& title) {
    return (title.empty() ? std::string("The game") : title) + " wouldn't start. Check the game opens from your Library, then try again.";
}
inline std::string online_unavailable_text(const std::string& system) {
    return system.empty() ? std::string("Online play isn't available for this game yet.") : "Online play isn't available for " + system + " yet.";
}
inline const char* connection_check_failed_text() { return "Couldn't start the connection check."; }
inline const char* remote_play_failed_text() { return "Couldn't start remote play."; }
inline const char* remote_play_missing_text() { return "Remote play files are missing. Update RetroHub."; }
inline const char* ps1_files_missing_text() { return "PlayStation online files are missing. Reinstall RetroHub."; }
inline const char* ps1_bios_own_text() { return "Use a BIOS dumped from your own console. RetroHub does not provide one."; }

/* ---- legacy wire texts (what is SENT to other players; unchanged from earlier versions) ---- */
inline std::string core_missing_wire(const std::string& core) { return "Emulator core '" + core + "' not found (cores folder)."; }
inline const char* core_missing_wire_nds() { return "Emulator core for Nintendo DS not found (cores folder)."; }
inline const char* start_failed_wire() { return "The game did not start on this PC."; }
inline const char* online_unavailable_wire() { return "Online play is not available for this system yet."; }
inline std::string remote_play_failed_wire(unsigned long err) { return "Could not start remote play (streamrun error " + std::to_string(err) + ")."; }
inline const char* remote_play_missing_wire() { return "Remote play isn't installed (streamrun.exe is missing) - update RetroHub."; }
inline const char* ps1_core_missing_wire() { return "The PlayStation emulator core (beetle_psx_libretro.dll) was not found in the cores folder."; }
inline const char* ps1_bios_missing_wire() { return "PlayStation online needs a BIOS file (scph5501.bin) on every PC, or the built-in OpenBIOS on every PC. Set it up in Settings > Playing."; }

inline bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
inline bool ends_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
/* Text to put on the wire for a launch error shown as `shown`: the fixed player-facing texts above that replaced older wording go out
 * as the older wording. Anything else goes out unchanged. */
inline std::string wire_for(const std::string& shown) {
    if (shown == ps1_files_missing_text()) return ps1_core_missing_wire();
    if (shown == remote_play_missing_text()) return remote_play_missing_wire();
    if (starts_with(shown, "PlayStation online needs a BIOS file") && ends_with(shown, ps1_bios_own_text())) return ps1_bios_missing_wire();
    return shown;
}

/* ---- host's view of a guest's np_fail ---- */

/* The line the HOST shows for "guest `name` could not start, reason `raw`". Known guest-directed sentences become third person; any
 * other text is shown as "<name> can't start: <raw>" (exactly what earlier versions showed). */
inline std::string host_view_of_guest_fail(const std::string& name_in, const std::string& raw) {
    const std::string name = name_in.empty() ? std::string("A guest") : name_in;
    const std::string cant = name + " can't start: ";
    if (starts_with(raw, "RetroHub can't find the game file for ")) return cant + "the game file is missing on " + name + "'s PC.";
    if (raw == "The host started a game you don't have.") return name + " doesn't have this game.";
    if (raw == "The host started a game, but a game is already running on this PC.") return cant + "a game is already running on " + name + "'s PC.";
    if (starts_with(raw, "Emulator core ") && raw.find("not found") != std::string::npos)
        return cant + "RetroHub on " + name + "'s PC is missing a file it needs.";
    if (raw == start_failed_wire()) return cant + "the game didn't open on " + name + "'s PC.";
    if (raw == ps1_core_missing_wire()) return cant + "the PlayStation online files are missing on " + name + "'s PC.";
    if (raw == ps1_bios_missing_wire()) return cant + name + "'s PC has no PlayStation BIOS set up.";
    if (starts_with(raw, "A GameCube session is still open on this PC")) return cant + "a GameCube game is still open on " + name + "'s PC.";
    if (starts_with(raw, "The host's ") && ends_with(raw, "ask them to update RetroHub."))
        return cant + "RetroHub on " + name + "'s PC can't read this launch. Updating RetroHub may help.";
    if (raw == online_unavailable_wire()) return cant + "online play isn't available for this game on " + name + "'s PC.";
    return cant + raw;
}

}  // namespace hublaunch
