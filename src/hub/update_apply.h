#pragma once
/* Native update applier: `RetroHub.exe --apply-update <staged dir> <install dir> <pid> [flags]`.
 *
 * Runs from a TEMP copy of RetroHub.exe (so the install's exe is not locked). Console-less and
 * window-less (RetroHub.exe is a GUI-subsystem exe and this path runs before any SDL/window
 * code). It waits for the old process(es) to exit, backs up every file it replaces, copies the
 * staged files over the install using UTF-16 paths, rolls back completely if any copy fails,
 * updates the HKCU uninstall DisplayVersion (setup installs), and relaunches RetroHub.
 *
 * Never touched: userdata\, saves, BattleShip.o2r, logs, updates\ (see is_excluded()).
 *
 * Flags: --version <tag>  --no-relaunch  --retries <n>  --retry-ms <ms>
 *        --started-marker <file>  --backup-dir <dir>
 * Exit code: 0 applied, 1 failed (rolled back), 2 usage. */

#include <string>
#include <vector>

namespace hubupdate {
namespace apply {

struct Options {
    std::wstring staged, install, backup;   // backup "" => %TEMP%\RetroHub-update-<pid>\backup
    unsigned long pid = 0;                   // process to wait for (0 = none)
    std::wstring version;                    // release tag, for DisplayVersion
    std::wstring started_marker;             // created as soon as the applier is running
    bool relaunch = true;
    bool game_active = false;                // --game-active: a game was running when the update started; relaunch RetroHub minimised (no focus)
    int retries = 10;                        // per-file copy attempts
    int retry_ms = 500;
    bool (*copy_hook)(const std::wstring& from, const std::wstring& to) = nullptr;   // tests only: replaces the copy
};

struct Result {
    bool ok = false;
    bool rolled_back = false;
    int copied = 0;
    std::wstring error;
};

/* ---- pure helpers (unit tested) ---- */
bool is_excluded(const std::wstring& rel);       // user data / logs / saves / BattleShip.o2r / updates
bool is_safe_rel(const std::wstring& rel);       // no "..", no drive/rooted paths
bool path_is_under(const std::wstring& dir, const std::wstring& path);   // case-insensitive
std::vector<std::wstring> list_staged(const std::wstring& staged);       // relative, backslashes, exclusions applied, sorted

/* ---- file phase: backup + copy + rollback (no process handling, no relaunch) ---- */
Result apply_files(const Options& o);

/* ---- full flow ---- */
int run(const Options& o);

/* Parses the process command line; if it is `--apply-update ...` runs the applier and returns
 * true with the exit code. Call first thing in WinMain. */
bool try_apply_mode(int* exit_code);

} // namespace apply
} // namespace hubupdate
