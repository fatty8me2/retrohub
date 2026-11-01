#pragma once
/* Native update applier: `RetroHub.exe --apply-update <staged dir> <install dir> <pid> [flags]`.
 *
 * Runs from a TEMP copy of RetroHub.exe (so the install's exe is not locked). Console-less and
 * window-less (RetroHub.exe is a GUI-subsystem exe and this path runs before any SDL/window
 * code). It waits for the old RetroHub to exit, then waits for anything else still running from the install
 * folder (a game, an emulator): it NEVER terminates those. With a game still running after a long wait it gives
 * up, changes nothing, relaunches the old RetroHub quietly and leaves a status note (exit code 3).
 * Otherwise it backs up every file it replaces (in %TEMP%\RetroHub-update-<pid>\backup, with a manifest), copies the
 * staged files over the install using UTF-16 paths and rolls back completely if any copy fails. When a restore
 * itself fails the backup is KEPT (it is then the only good copy) and startup recovery (recover_interrupted)
 * repairs from it later. It updates the HKCU uninstall DisplayVersion (setup installs) and relaunches RetroHub.
 * Every failure writes userdata\update_status.json, which the next RetroHub start shows once as a bell entry
 * (never a window), and the staged files are kept so a retry needs no download (updater.cpp reuses them).
 *
 * Never touched: userdata\, saves, BattleShip.o2r, logs, updates\ (see is_excluded()).
 *
 * Flags: --version <tag>  --from <tag>  --no-relaunch  --retries <n>  --retry-ms <ms>  --game-active
 *        --started-marker <file>  --backup-dir <dir>  --wait-secs <n>  --hub-wait-secs <n>
 * Exit code: 0 applied, 1 failed (rolled back), 2 usage, 3 postponed (a game was still running; nothing changed). */

#include <string>
#include <vector>

namespace hubupdate {
namespace apply {

struct Options {
    std::wstring staged, install, backup;   // backup "" => %TEMP%\RetroHub-update-<pid>\backup
    unsigned long pid = 0;                   // process to wait for (0 = none)
    std::wstring version;                    // release tag, for DisplayVersion
    std::wstring from_version;               // the tag being replaced: recovery only restores a backup onto the version it came from
    std::wstring started_marker;             // created as soon as the applier is running
    bool relaunch = true;
    bool game_active = false;                // --game-active: a game was running when the update started; relaunch RetroHub minimised (no focus)
    int retries = 10;                        // per-file copy attempts
    int retry_ms = 500;
    int restore_retries = 20;                // rollback restores try much harder: a failed restore is what costs a player files
    int restore_retry_ms = 500;
    unsigned wait_games_secs = 1200;         // how long to wait for a game / emulator / helper from the install folder before giving up
    unsigned hub_wait_secs = 300;            // how long the OLD RetroHub gets to exit; it is never ended either: after this the apply is postponed
    std::wstring tmp_root;                   // where RetroHub-update-<pid>\ goes ("" => %TEMP%); tests
    std::wstring status_path;                // "" => <install>\userdata\update_status.json
    bool (*copy_hook)(const std::wstring& from, const std::wstring& to) = nullptr;   // tests only: replaces the copy
};

struct Result {
    bool ok = false;
    bool rolled_back = false;                // every touched file is back (or was new and removed)
    bool backup_kept = false;                // a restore failed: the backup dir holds the only good copy of unrestored[]
    std::wstring backup_dir;
    std::vector<std::wstring> unrestored;    // relative paths whose restore failed
    int copied = 0;
    unsigned long err = 0;                   // Windows error of the failing step
    std::wstring error;
};

/* ---- pure helpers (unit tested) ---- */
bool is_excluded(const std::wstring& rel);       // user data / logs / saves / BattleShip.o2r / updates
bool is_safe_rel(const std::wstring& rel);       // no "..", no drive/rooted paths
bool path_is_under(const std::wstring& dir, const std::wstring& path);   // case-insensitive
std::vector<std::wstring> list_staged(const std::wstring& staged);       // relative, backslashes, exclusions applied, sorted
std::string plain_reason(unsigned long win_error);   // "a file was in use by another program", "the disk is full", ...

/* ---- processes from the install folder (a game, an emulator ...) ---- */
struct Running { unsigned long pid = 0; std::wstring image; };
/* Every process whose image lives under `dir`, except the pids in `except` and this process. */
std::vector<Running> processes_under(const std::wstring& dir, const std::vector<unsigned long>& except = {});
enum class WaitOutcome { Clear, TimedOut };
/* Waits for the old RetroHub (o.pid, up to hub_wait_secs), then for everything else under o.install
 * (up to wait_games_secs). Nothing is ever terminated. *blocker names the first process still running on TimedOut. */
WaitOutcome wait_for_old_processes(const Options& o, std::wstring* blocker);

/* ---- update status note (userdata\update_status.json): one line for the bell on the next start ---- */
struct Status {
    std::string result;     // "failed" (rolled back) | "postponed" (a game was running) | "rollback_failed" (backup kept) | "interrupted" | "repaired"
    std::string version;    // the version that was being installed
    std::string reason;     // plain language
    std::string detail;     // technical (error code, file)
    std::string backup;     // backup dir when kept
    long long at = 0;       // unix seconds
};
std::wstring default_status_path(const std::wstring& install);
bool write_status(const std::wstring& path, const Status& s);
bool read_status(const std::wstring& path, Status* s);
std::string status_line(const Status& s);        // the bell entry text, short enough not to be cut off
/* Reads the status note and deletes the file ("shows it once"). false when there is none. */
bool take_status(const std::wstring& path, Status* s);

/* ---- file phase: backup + copy + rollback (no process handling, no relaunch) ---- */
Result apply_files(const Options& o);

/* ---- startup recovery: leftover %TEMP%\RetroHub-update-<pid>\ dirs of THIS install ---- */
struct RecoverReport {
    int found = 0;        // RetroHub-update-* dirs seen
    int active = 0;       // an applier is still running: left alone
    int other = 0;        // another install's, or no readable manifest: left alone
    int cleaned = 0;      // nothing to repair (no backup, or everything already matches): removed
    int repaired = 0;     // files put back from the backup, dir removed
    int partial = 0;      // some files could not be put back: backup kept
    int stale = 0;        // the install is no longer on the version the backup came from: removed, never restored (would downgrade)
    int restored_files = 0;
    std::vector<std::string> notes;   // one-line bell texts for the next start
};
RecoverReport recover_interrupted(const std::wstring& install, const std::string& current_version, const std::wstring& tmp_root = std::wstring());

/* ---- full flow ---- */
int run(const Options& o);

/* Parses the process command line; if it is `--apply-update ...` runs the applier and returns
 * true with the exit code. Call first thing in WinMain. */
bool try_apply_mode(int* exit_code);

} // namespace apply
} // namespace hubupdate
