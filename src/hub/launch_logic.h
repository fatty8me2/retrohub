#pragma once
/* Pure launch decisions and command-line pieces shared by play_entry() in launch.cpp (unit tested by hub_launch_logic_test).
 * Header-only: no UI, no processes, no files. Everything here used to be inline in launch.cpp; the behaviour is unchanged. */
#include <algorithm>
#include <string>

#include "library.h"
#include "winutil.h"

namespace hubui {

/* Libretro core (file name without "_libretro.dll") a retrorun system uses; "" for a system retrorun does not play this way.
 * `gb_core` is the player's Game Boy / Color core setting. */
inline std::string retro_core_name(hub::System s, const std::string& gb_core) {
    switch (s) {
        case hub::System::GBA: return "mgba";
        case hub::System::GB: case hub::System::GBC: return gb_core;
        case hub::System::NES: return "nestopia";
        case hub::System::SNES: return "snes9x";
        case hub::System::NDS: return "melondsds";
        default: return std::string();
    }
}

/* Game window caption: "<title> - RetroHub" (UTF-8 em dash), quoted for the command line. */
inline std::string title_arg(const std::string& game_title) {
    std::string t = game_title;
    t.erase(std::remove(t.begin(), t.end(), '"'), t.end());   // a quote would break the argument
    return " --title " + winutil::quote_arg(t + " \xE2\x80\x94 RetroHub");
}

/* retrorun names the DS save <stem>.srm after the ROM file. A zipped ROM is extracted as rom.<ext>, so pass the inner name's stem. */
inline std::string save_stem_arg(const std::string& save_stem) {
    return save_stem.empty() ? std::string() : " --save-stem " + winutil::quote_arg(save_stem);
}

/* Drop " --resume" (a real console startup replaces the resume point). Leaves every other argument alone. */
inline std::string erase_resume_arg(std::string args) {
    size_t k = args.find(" --resume");
    if (k != std::string::npos) args.erase(k, 9);
    return args;
}

/* n64run --resume loads <state-dir>/auto.state, so it needs --state-dir; add it when resume is there and no state dir was given. */
inline std::string ensure_state_dir_for_resume(std::string args, const std::string& states) {
    if (args.find(" --resume") != std::string::npos && args.find("--state-dir ") == std::string::npos)
        args += " --state-dir " + winutil::quote_arg(states);
    return args;
}

/* Smash Remix / Smash 64 ROMs report each finished VS match to a result file (rivals, party mode, brackets). */
inline bool n64_wants_result_file(const std::string& catalog_id) { return catalog_id == "remix" || catalog_id == "smash64"; }

/* The first part of an n64run command line (the rest: controls, settings, cheats, quick menu, resume). */
inline std::string n64_base_args(const std::string& rom, const std::string& catalog_id, const std::string& battery_dir, const std::string& title) {
    return "--rom " + winutil::quote_arg(rom) + " --game " + (catalog_id.empty() ? "unknown" : catalog_id) +
           " --save-dir " + winutil::quote_arg(battery_dir) + title_arg(title);
}

/* The first part of a retrorun command line. `resume` is the ready-made " --resume" fragment ("" when none; DS never resumes).
 * `state_dir` empty = no --state-dir (not used by any current caller, but keeps the builder honest). */
inline std::string retro_base_args(const std::string& core, const std::string& rom, const std::string& system_short,
                                   const std::string& battery_dir, const std::string& state_dir, const std::string& resume,
                                   const std::string& title, const std::string& save_stem) {
    std::string a = "--core " + winutil::quote_arg(core) + " --rom " + winutil::quote_arg(rom) + " --system " + system_short +
                    " --save-dir " + winutil::quote_arg(battery_dir);
    if (!state_dir.empty()) a += " --state-dir " + winutil::quote_arg(state_dir);
    return a + resume + title_arg(title) + save_stem_arg(save_stem);
}

}  // namespace hubui
