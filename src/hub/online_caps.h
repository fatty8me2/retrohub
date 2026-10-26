#pragma once
/* Which systems can be played in a room. Pure (no UI state) so a console test can check it against launch_online.cpp's branches. */
#include "games.h"
#include "library.h"

namespace hubcaps {

/* Systems launch_online() has a real online branch for. KEEP IN SYNC with launch_online.cpp: add the system here when you add a branch
 * (the test hub_online_caps_test then forces supports_online to follow). PS2 (PCSX2 has no netplay) is deliberately absent. Wii U = remote play (streamed). */
inline bool system_has_online_launch(hub::System s) {
    switch (s) {
        case hub::System::N64: case hub::System::GBA: case hub::System::GB: case hub::System::GBC:
        case hub::System::N3DS: case hub::System::GC: case hub::System::NDS: case hub::System::PS1:
        case hub::System::NES: case hub::System::SNES: case hub::System::WiiU:
            return true;
        default: return false;
    }
}

/* Remote play: the host runs the emulator and streams video to guests (streamrun), guests send pad input back. Guests need no copy
 * of the game. Today: Wii U (Cemu) only. */
inline bool remote_play_system(hub::System s) { return s == hub::System::WiiU; }
inline bool remote_play_game(hub::System s, const hub::GameInfo*) { return remote_play_system(s); }
inline const char* remote_play_banner() { return "Mickey streams the game to you - you don't need your own copy."; }

/* Systems where only catalog-vetted games are KNOWN to work online. Every game of these systems is still offered in rooms (GC: any disc via
 * Dolphin NetPlay; DS: any game via ds-link; PS1: any game via rollback), but a game outside the catalog (or not flagged online) is tagged
 * "Untested online" because it may be single-player or need peripherals the link cannot carry. */
inline bool online_needs_catalog_flag(hub::System s) {
    return s == hub::System::GC || s == hub::System::NDS || s == hub::System::PS1 || s == hub::System::WiiU;
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
    switch (s) {
        case hub::System::PS2: return "PS2 online isn't supported yet";
        default: return "Online play isn't available for this game";
    }
}

/* Room player cap for a game that has no catalog entry. GB/GBC link cable, DS wireless and PS1 two-pad games default to 2;
 * the rest to 4 (GameCube has four ports, N64/GBA/3DS rooms are up to four). The catalog's online_max wins when present. */
inline int default_online_max(hub::System s) {
    switch (s) {
        case hub::System::GB: case hub::System::GBC: case hub::System::NDS: case hub::System::PS1:
        case hub::System::NES: case hub::System::SNES: case hub::System::WiiU: return 2;   // Wii U: 2 unless the catalog lists more couch pads   // NES/SNES: two pads unless the catalog says multitap / Four Score
        default: return 4;
    }
}

}  // namespace hubcaps
