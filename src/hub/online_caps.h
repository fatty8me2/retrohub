#pragma once
/* Which systems can be played in a room. Pure (no UI state) so a console test can check it against launch_online.cpp's branches. */
#include <map>
#include <string>

#include "games.h"
#include "library.h"

namespace hubcaps {

/* Systems launch_online() has a real online branch for. KEEP IN SYNC with launch_online.cpp: add the system here when you add a branch
 * (the test hub_online_caps_test then forces supports_online to follow). PS2 goes through retrorun + the LRPS2 core (docs/PS2_ONLINE.md); Wii U = remote play (streamed). */
inline bool system_has_online_launch(hub::System s) {
    switch (s) {
        case hub::System::N64: case hub::System::GBA: case hub::System::GB: case hub::System::GBC:
        case hub::System::N3DS: case hub::System::GC: case hub::System::NDS: case hub::System::PS1: case hub::System::PS2:
        case hub::System::NES: case hub::System::SNES: case hub::System::WiiU:
            return true;
        default: return false;
    }
}

/* Version id in the room hello: "v:<catalog id>" -> the copy's content key. Only for the cartridge systems whose key follows the ROM content (GBA/NES/SNES CRC32, GB/GBC
 * header checksum), so a USA and a Japanese copy differ. NDS: game code + ROM version + header CRC16 (catches region/revision, not a patched body). */
constexpr const char* kVerPrefix = "v:";
inline bool has_version_id(hub::System s) {
    return s == hub::System::GB || s == hub::System::GBC || s == hub::System::GBA || s == hub::System::NES || s == hub::System::SNES || s == hub::System::NDS;
}
inline void add_version_id(std::map<std::string, std::string>* games, hub::System s, const std::string& catalog_id, const std::string& key) {
    if (games && !catalog_id.empty() && !key.empty() && has_version_id(s)) games->emplace(std::string(kVerPrefix) + catalog_id, key);
}

/* Remote play: the host runs the emulator and streams video to guests (streamrun), guests send pad input back. Guests need no copy
 * of the game. Today: Wii U (Cemu) only. */
inline bool remote_play_system(hub::System s) { return s == hub::System::WiiU; }
inline bool remote_play_game(hub::System s, const hub::GameInfo*) { return remote_play_system(s); }
inline std::string remote_play_banner(const std::string& host_name = std::string()) {
    return (host_name.empty() ? std::string("The host") : host_name) + " streams the game to you - you don't need your own copy.";
}

/* ---- GameCube NetPlay session lifetime (onlineflow-H5) ----
 * The hubnet forwarder carries all of Dolphin's traffic and is only pumped by gc_tick(). It must keep ticking for as long as a session
 * exists, whatever the room page or the lobby socket is doing (a lobby blip or "Leave room" used to stall everyone's match). */
/* gco_active stays true until gc_tick sees Dolphin gone and resets it, so a dead Dolphin still ticks once to end the session cleanly. */
inline bool gc_session_alive(bool gco_active, bool forwarder_active) { return gco_active || forwarder_active; }
/* True when the room-bound tick must NOT tick it again this frame (already pumped at the top of the frame). */
inline bool gc_tick_needed_in_room(bool already_ticked) { return !already_ticked; }

/* ---- Cores a remote host may name in its launch message (onlineflow-M12) ----
 * Only the GB/GBC branch of launch_online() uses the host's core name; everything else picks its core itself. A name is accepted only when
 * it is a bare file name (no separators, no "..", no drive or UNC prefix) AND a known RetroHub core for that system. Empty = "use my own setting". */
inline bool core_name_is_bare(const std::string& n) {
    if (n.empty() || n.size() > 40 || n == "." || n == "..") return false;
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;   // rejects \ / : . spaces and control characters in one go
    }
    return true;
}
inline bool core_known_for(hub::System s, const std::string& n) {
    switch (s) {
        case hub::System::GB: case hub::System::GBC: return n == "sameboy" || n == "sameboy_rb" || n == "gambatte";
        case hub::System::GBA: return n == "mgba" || n == "mgba_link";
        case hub::System::NDS: return n == "melondsds";
        case hub::System::NES: return n == "nestopia";
        case hub::System::SNES: return n == "snes9x";
        default: return false;
    }
}
inline bool core_from_host_ok(hub::System s, const std::string& n) { return core_name_is_bare(n) && core_known_for(s, n); }
inline const char* core_refused_text() { return "The host's game needs an emulator core RetroHub doesn't know."; }

/* ---- 3DS Azahar join helper result (onlineflow-M3) ----
 * The helper writes "joined" or "error:<why>" to its status file. No answer at all (helper gone, or too slow) is a failure too. */
enum class JoinVerdict { Pending, Joined, Error, HelperGone, TimedOut };
inline JoinVerdict join_verdict(bool helper_started, bool helper_alive, const std::string& status_text, unsigned long long elapsed_ms, unsigned long long timeout_ms = 150000, std::string* detail = nullptr) {
    std::string last;
    size_t p = 0;
    while (p <= status_text.size()) {
        size_t q = status_text.find('\n', p);
        if (q == std::string::npos) q = status_text.size();
        std::string l = status_text.substr(p, q - p);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l == "joined" || l.rfind("error:", 0) == 0) last = l;
        p = q + 1;
    }
    if (last == "joined") return JoinVerdict::Joined;
    if (!last.empty()) { if (detail) *detail = last.substr(6); return JoinVerdict::Error; }
    if (!helper_started || !helper_alive) return JoinVerdict::HelperGone;
    if (elapsed_ms > timeout_ms) return JoinVerdict::TimedOut;
    return JoinVerdict::Pending;
}
inline std::string join_failure_text(JoinVerdict v) {
    switch (v) {
        case JoinVerdict::Error: return "Azahar couldn't connect to the room. It is running on its own - close it and try again.";
        case JoinVerdict::HelperGone: return "RetroHub couldn't link Azahar to the room. It is running on its own - close it and try again.";
        case JoinVerdict::TimedOut: return "Azahar took too long to join the room. It is running on its own - close it and try again.";
        default: return std::string();
    }
}

/* Systems where only catalog-vetted games are KNOWN to work online. Every game of these systems is still offered in rooms (GC: any disc via
 * Dolphin NetPlay; DS: any game via ds-link; PS1: any game via rollback), but a game outside the catalog (or not flagged online) is tagged
 * "Untested online" because it may be single-player or need peripherals the link cannot carry. */
inline bool online_needs_catalog_flag(hub::System s) {
    return s == hub::System::GC || s == hub::System::NDS || s == hub::System::PS1 || s == hub::System::PS2 || s == hub::System::WiiU;
}

/* Everything that can technically work online is capable; the catalog only changes the "untested" tag. */
inline bool online_capable(hub::System s, const hub::GameInfo* /*cat*/) { return system_has_online_launch(s); }

/* Capable, but not vetted: shown with an "Untested online" tag. */
inline bool online_untested(hub::System s, const hub::GameInfo* cat) {
    return online_capable(s, cat) && online_needs_catalog_flag(s) && !(cat && cat->online);
}

/* Short reason a system cannot play online at all ("" = it can). The picker still lists such games, greyed. */
inline const char* online_unsupported_reason(hub::System s) {
    if (system_has_online_launch(s)) return "";
    return "Online play isn't available for this game";
}

/* Room player cap for a game that has no catalog entry: what the system's online path can carry for an unvetted game. GB/GBC link cable and DS
 * wireless rooms are 2; NES/SNES are 2 because 3-4 players need the Four Score / multitap, which is only known to work for the catalog's games.
 * Everything else takes the console's four pads: GameCube (Dolphin NetPlay), N64, GBA/3DS rooms, PS1 and PS2 (emulated multitap switched on for 3-4
 * players), Wii U (the host's GamePad + up to 3 guests' Pro Controllers on DSU slots, hubrp::guest_count). The catalog's online_max wins when present. */
inline int default_online_max(hub::System s) {
    switch (s) {
        case hub::System::GB: case hub::System::GBC: case hub::System::NDS:
        case hub::System::NES: case hub::System::SNES: return 2;
        default: return 4;
    }
}

/* Players one PC can seat for a game that has no catalog entry (the "1-N players" label, the port strip before Play, the party couch pool). These are the
 * old values (the room cap) except where the old value was too low for what the console takes: PS1 and Wii U take four pads (emulated multitap /
 * Cemu players 1-4); PSP is one pad. NES/SNES stay at 2 until the catalog lists the game. */
inline int default_local_max(hub::System s) {
    switch (s) {
        case hub::System::PSP: return 1;
        case hub::System::Switch: return 1;   // Lichii: player 1 only (RetroHub writes one controller)
        case hub::System::GB: case hub::System::GBC: case hub::System::NDS: case hub::System::NES: case hub::System::SNES: return 2;
        default: return 4;   // N64, GC, GBA, 3DS (as before), PS1 and Wii U (raised from 2), PS2
    }
}

/* Buzz! quiz games (PS1/PS2) are played with USB buzzers that standalone PCSX2 emulates (pcsx2_ini_buzz) but the room's LRPS2 core has off, so a
 * room could start them with nobody able to answer. */
inline bool is_buzz_game(const hub::GameInfo* cat, const std::string& title) {
    return (cat && cat->id.compare(0, 5, "buzz_") == 0) || title.compare(0, 5, "Buzz!") == 0;
}

/* "" = the game can be played in a room; otherwise why it cannot (the picker lists it greyed with this text). Only for systems that can play online at all. */
inline std::string online_game_block(hub::System s, const hub::GameInfo* cat, const std::string& title) {
    if ((s == hub::System::PS2 || s == hub::System::PS1) && is_buzz_game(cat, title))
        return "Buzz! needs its USB buzzers, which online rooms don't have. Play it on this PC from the library.";
    return "";
}

/* One line saying why a room for this game holds at most `cap` players ("" = the console's four and nothing to explain). Shown on the
 * room's game card and after "too many players" (`only_new_info`: "" when the note would only repeat the number). */
inline std::string online_limit_note(hub::System s, const hub::GameInfo* cat, int cap, bool only_new_info = false) {
    if (cat && hub::link_cable_game(cat->id)) return "Link-cable game: exactly 2 players, one per console.";
    if (cap >= 4) return "";
    if (cap <= 1) return cat ? "This game is single-player, so a room can't play it with others." : "";
    if (cat) {
        if (cat->online_max > 0 && cat->online_max < cat->max_players)
            return "Online is limited to " + std::to_string(cap) + " players here (the game itself supports " + std::to_string(cat->max_players) + ").";
        return only_new_info ? std::string() : "This game supports at most " + std::to_string(cap) + " players.";   // "too many players (max N)" says it already
    }
    switch (s) {
        case hub::System::NES: return "Unlisted NES games are limited to 2 players: 3-4 need the Four Score, which only the listed games are known to use.";
        case hub::System::SNES: return "Unlisted SNES games are limited to 2 players: 3-4 need the multitap, which only the listed games are known to use.";
        case hub::System::GB: case hub::System::GBC: return "Game Boy rooms are limited to 2 players (the link cable).";
        case hub::System::NDS: return "Unlisted DS games are limited to 2 players (DS wireless).";
        default: return "";
    }
}

/* Seats retrorun may drive on ONE PC (`retrorun --max-local`): the catalog's max_players, else default_local_max, except that an uncatalogued NES / SNES / PS1
 * game gets 2 (it is assumed to be a one- or two-player game). Retrorun plays with min(real pads, seats) and only switches the 4-pad adapter (Four Score,
 * multitap) on for 3 or more, so a 1- or 2-player game never boots with an adapter. Uncatalogued PS2 games keep 4 (Gauntlet: Dark Legacy). */
inline int runner_local_seats(hub::System s, const hub::GameInfo* cat) {
    if (cat && cat->max_players > 0) return cat->max_players;
    switch (s) {
        case hub::System::NES: case hub::System::SNES: case hub::System::PS1: return 2;
        default: return default_local_max(s);
    }
}

}  // namespace hubcaps
