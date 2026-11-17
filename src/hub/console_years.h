#pragma once
/* The year each console first came out (anywhere in the world: the Famicom for the NES, the Super Famicom for the SNES), and the
 * oldest-first order the Library desk lists its console groups in (desk_logic.h desk_console_order; the group headers show the year).
 * Keyed by console id (library.h system_short, plus "switch2"). Pure: no other hub headers. Unit-tested in test_desk.cpp (hub_desk_test),
 * which also checks that every console in hubui::console_order() has a year here.
 *   launch_year:  1983 for "nes"; 0 for an id that is not in the table
 *   launch_less:  oldest first; a console without a year after every one with a year; the same year: home consoles before handhelds,
 *                 then the id A-Z (GameCube before Game Boy Advance in 2001, DS before PSP in 2004) */

#include <algorithm>
#include <string>
#include <vector>

namespace hub {

struct ConsoleLaunch {
    const char* id;
    int year;
    bool handheld;   // Switch / Switch 2 count as home consoles (they dock)
};

inline constexpr ConsoleLaunch kConsoleLaunches[] = {
    {"nes", 1983, false},       // Famicom, Japan, July 1983
    {"gb", 1989, true},         // Japan, April 1989
    {"snes", 1990, false},      // Super Famicom, Japan, November 1990
    {"ps1", 1994, false},       // Japan, December 1994
    {"n64", 1996, false},       // Japan, June 1996
    {"gbc", 1998, true},        // Japan, October 1998
    {"ps2", 2000, false},       // Japan, March 2000
    {"gba", 2001, true},        // Japan, March 2001
    {"gc", 2001, false},        // Japan, September 2001
    {"nds", 2004, true},        // North America, November 2004
    {"psp", 2004, true},        // Japan, December 2004
    {"3ds", 2011, true},        // Japan, February 2011
    {"wiiu", 2012, false},      // North America, November 2012
    {"switch", 2017, false},    // worldwide, March 2017
    {"switch2", 2025, false},   // worldwide, June 2025
};

inline const ConsoleLaunch* console_launch(const std::string& id) {
    for (const ConsoleLaunch& c : kConsoleLaunches)
        if (id == c.id) return &c;
    return nullptr;
}
inline int launch_year(const std::string& id) {
    const ConsoleLaunch* c = console_launch(id);
    return c ? c->year : 0;
}
/* "1983" for a header; "" for a console without a year. */
inline std::string launch_year_text(const std::string& id) {
    const int y = launch_year(id);
    return y > 0 ? std::to_string(y) : std::string();
}

inline bool launch_less(const std::string& a, const std::string& b) {
    const ConsoleLaunch* x = console_launch(a);
    const ConsoleLaunch* y = console_launch(b);
    if ((x != nullptr) != (y != nullptr)) return x != nullptr;   // a console without a year goes last
    if (x && y) {
        if (x->year != y->year) return x->year < y->year;
        if (x->handheld != y->handheld) return !x->handheld;    // the same year: the home console first
    }
    return a < b;
}
/* The ids, oldest console first (launch_less). */
inline std::vector<std::string> by_launch(std::vector<std::string> ids) {
    std::stable_sort(ids.begin(), ids.end(), launch_less);
    return ids;
}

}  // namespace hub
