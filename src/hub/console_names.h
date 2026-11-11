#pragma once
/* The one place that says what a console is called on screen.
 *   full_name:  "Nintendo 64", "PlayStation", "Game Boy Advance"   (headings, sentences, pills)
 *   short_name: "N64", "PS1", "GBA"                                (chips, badges, tight spaces)
 * Both follow the canonical system table in library.h (System / system_short). Nintendo Switch 2 is a console of its own on
 * screen, so every function takes an optional nx2 flag, and there is a console-id overload ("switch2" is a console id).
 * Pure; unit-tested in test_console_names.cpp. */

#include "library.h"

#include <string>

namespace hub {

inline const char* full_name(System s, bool nx2 = false) {
    if (s == System::Switch && nx2) return "Nintendo Switch 2";
    return s == System::Unknown ? "" : system_name(s);
}

inline const char* short_name(System s, bool nx2 = false) {
    switch (s) {
    case System::N64:    return "N64";
    case System::GBA:    return "GBA";
    case System::GB:     return "GB";
    case System::GBC:    return "GBC";
    case System::GC:     return "GameCube";
    case System::N3DS:   return "3DS";
    case System::WiiU:   return "Wii U";
    case System::NDS:    return "DS";
    case System::PS1:    return "PS1";
    case System::PS2:    return "PS2";
    case System::NES:    return "NES";
    case System::SNES:   return "SNES";
    case System::PSP:    return "PSP";
    case System::Switch: return nx2 ? "Switch 2" : "Switch";
    default:             return "";
    }
}

/* By console id (not named full_name / short_name: the hub also has a global short_name(game id)). An unknown id gives "". */
inline const char* full_name_id(const std::string& console_id) {
    return console_is_switch2(console_id) ? full_name(System::Switch, true) : full_name(system_from_short(console_id));
}
inline const char* short_name_id(const std::string& console_id) {
    return console_is_switch2(console_id) ? short_name(System::Switch, true) : short_name(system_from_short(console_id));
}

}  // namespace hub
