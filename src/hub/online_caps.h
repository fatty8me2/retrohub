#pragma once
/* Which systems can be played in a room. Pure (no UI state) so a console test can check it against launch_online.cpp's branches. */
#include "games.h"
#include "library.h"

namespace hubcaps {

/* Systems launch_online() has a real online branch for. KEEP IN SYNC with launch_online.cpp: add the system here when you add a branch
 * (the test hub_online_caps_test then forces supports_online to follow). PS2 (PCSX2 has no netplay) and Wii U are deliberately absent. */
inline bool system_has_online_launch(hub::System s) {
    switch (s) {
        case hub::System::N64: case hub::System::GBA: case hub::System::GB: case hub::System::GBC:
        case hub::System::N3DS: case hub::System::GC: case hub::System::NDS: case hub::System::PS1:
            return true;
        default: return false;
    }
}

/* Systems whose online play is opt-in per catalog game (the catalog `online` flag). Other netplay systems accept any library game.
 * GC/NDS/PS1 are catalog-gated because only vetted games have a working multi-player mode over the link (PS1: only titles with
 * 2+ DualShock players; unknown discs may be single-player or need Buzz/light-gun peripherals that rollback cannot carry). */
inline bool online_needs_catalog_flag(hub::System s) {
    return s == hub::System::GC || s == hub::System::NDS || s == hub::System::PS1;
}

inline bool online_capable(hub::System s, const hub::GameInfo* cat) {
    if (!system_has_online_launch(s)) return false;
    if (online_needs_catalog_flag(s)) return cat && cat->online;
    return true;
}

}  // namespace hubcaps
