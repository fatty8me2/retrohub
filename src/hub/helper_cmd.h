#pragma once
/* Command line that runs one of the bundled helper scripts through rh-helper.exe (next to RetroHub.exe).
 * The hub never starts a script interpreter itself: rh-helper does, from <exe dir>\data\helpers\ only. */
#include <string>

#include "../helper/rh_helper_logic.h"
#include "winutil.h"

namespace hubhelper {

/* `"<exe dir>\rh-helper.exe" run <script> <args>`; args is the script's already-quoted parameter text. */
inline std::string run_command(const char* script, const std::string& args) {
    return winutil::quote_arg(winutil::exe_dir() + "rh-helper.exe") + " run " + script + (args.empty() ? "" : " " + args);
}

}  // namespace hubhelper
