#pragma once
/* "The emulator crashed" note: pure logic (no UI, no process calls; unit-tested by hub_crashnote_test).
 *
 * When an external emulator's session ends, launch.cpp hands the process exit code here. classify() tells a crash (a Windows
 * NTSTATUS fault such as 0xC0000005) from a normal exit and from the codes RetroHub itself causes. find_dump() looks for the
 * Windows Error Reporting LocalDumps file <exe>.<pid>.dmp (only exists when the user enabled LocalDumps). */

#include <cstdint>
#include <string>

namespace hubcrash {

enum class Verdict {
    Normal,      // closed on its own, or by a console / user kill (Ctrl+C, End task): nothing to report
    HubEnded,    // the exit code RetroHub's own hang watch gives when it ends the emulator's job (TerminateJobObject(job, 0)): never a crash
    Crash,       // a fault: access violation, stack overflow, heap corruption, breakpoint, unhandled C++ exception ...
};

/* The exit code RetroHub passes to TerminateJobObject / TerminateProcess when it ends an emulator itself (launch.cpp lichii_hang_watch). */
constexpr uint32_t kHubEndExitCode = 0;

Verdict classify(uint32_t exit_code);
inline bool is_crash(uint32_t exit_code) { return classify(exit_code) == Verdict::Crash; }

std::string hex_code(uint32_t exit_code);   // "0xC0000005"

/* %LOCALAPPDATA%\CrashDumps (with a trailing backslash), "" when LOCALAPPDATA is unset. */
std::string default_dumps_dir();
/* <dumps_dir>\<exe_name>.<pid>.dmp when that file exists ("" otherwise). The exe name is matched case-insensitively. dumps_dir is injectable for tests. */
std::string find_dump(const std::string& dumps_dir, const std::string& exe_name, unsigned long pid);

struct Crash {
    std::string emulator;     // "Fritchii"
    std::string version;      // pinned release version, "" when unknown
    std::string exe_name;     // "lichii.exe"
    std::string game;         // game title ("" = unknown)
    uint32_t exit_code = 0;
    unsigned long pid = 0;
    long long time_unix = 0;  // when the session ended
    std::string dump_path;    // "" = no crash dump found
    std::string log_dir;      // the emulator's own log folder ("" = none offered)
};

std::string headline(const Crash& c);        // "<Emulator> stopped unexpectedly while running <game>."
std::string details_text(const Crash& c);    // multi-line text for "Copy details"
std::string folder_of(const std::string& path);   // the directory part of a file path ("" if none)

}
