#pragma once
/* Azahar 2126.2's Qt frontend ignores "-m" ("--multiplayer is not yet implemented for the Qt frontend"), so joining the
 * room is done through its own Multiplayer > Direct Connect to Room dialog, driven with UI Automation by a small
 * PowerShell helper that runs next to the emulator process (same desktop). Verified live: tools n3ds_live. */
#include <string>

#include "netplay_3ds_run.h"

namespace hubnp3ds {

const char* join_driver_script();

/* Pure: the helper's command-line arguments. */
std::string join_driver_args(unsigned long azahar_pid, const std::string& status_file, const std::string& nick,
                             const std::string& password, const std::string& host, int port, int timeout_s);

#ifdef _WIN32
/* Starts the helper (hidden, optionally on a given desktop name). Returns its pid or 0. */
unsigned long start_join_driver(unsigned long azahar_pid, const std::string& work_dir, const std::string& nick,
                                const std::string& password, const std::string& host, int port, const std::wstring& desktop,
                                std::string* status_file);
#endif

/* Reads the helper's status file: "joined" once the room accepted us, "error:..." on failure, else "". */
std::string join_state(const std::string& status_text);

}  // namespace hubnp3ds
