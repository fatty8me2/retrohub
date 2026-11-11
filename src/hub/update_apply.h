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
 *        --started-marker <file>  --backup-dir <dir>  --wait-secs <n>  --hub-wait-secs <n>  --hub-only --smash-sha256 <hex>
 *        --boot-wait-secs <n>  --boot-cap-secs <n>  --relaunch-no-window
 * Exit code: 0 applied (and the new RetroHub started), 1 failed (rolled back), 2 usage, 3 postponed (a game was still running; nothing
 * changed), 4 applied but the new RetroHub never confirmed (still running at the cap, or it ended without the marker but not like a failed start: kept, backup kept), 5 applied, new RetroHub did not start, rolled back,
 * 6 the boot rollback could not restore everything (backup kept for startup recovery). See "boot check" below. */

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
    std::wstring smash_sha256;               // --smash-sha256: with hub_only, the SHA-256 the installed BattleShip.exe must still have (else the apply is refused untouched)
    bool hub_only = false;                   // --hub-only: the staged files are RetroHub's part (RetroHub-windows.zip); Smash Classic's files are never replaced
    bool game_active = false;                // --game-active: a game was running when the update started; relaunch RetroHub minimised (no focus)
    int retries = 10;                        // per-file copy attempts
    int retry_ms = 500;
    int restore_retries = 20;                // rollback restores try much harder: a failed restore is what costs a player files
    int restore_retry_ms = 500;
    unsigned wait_games_secs = 1200;         // how long to wait for a game / emulator / helper from the install folder before giving up
    unsigned hub_wait_secs = 300;            // how long the OLD RetroHub gets to exit; it is never ended either: after this the apply is postponed
    bool keep_backup = false;                // apply_files only: keep the backup dir after a successful copy (the boot watch needs it)
    unsigned boot_wait_secs = 45;            // run(): after this long without the "boot ok" marker a slow start is only logged; the new RetroHub is never ended
    unsigned boot_cap_secs = 600;            // run(): hard cap while the new RetroHub is still running without the marker: stop watching, change nothing
    bool relaunch_no_window = false;         // --relaunch-no-window (tests only): the relaunched exe is a console stub, so start it without a console window
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
    std::vector<std::wstring> added;         // files the apply created that did not exist before (a boot rollback removes exactly these)
    bool smash_changed = false;             // hub-only apply refused: the installed BattleShip.exe is not the one the update was made for (nothing touched)
    int copied = 0;
    unsigned long err = 0;                   // Windows error of the failing step
    std::wstring error;
};

/* ---- pure helpers (unit tested) ---- */
bool is_excluded(const std::wstring& rel);       // user data / logs / saves / BattleShip.o2r / updates
bool is_safe_rel(const std::wstring& rel);       // no "..", no drive/rooted paths
bool path_is_under(const std::wstring& dir, const std::wstring& path);   // case-insensitive
/* Smash Classic's own files (BattleShip.exe, torch.exe, f3d.o2r, config.yml, yamls\, assets\, .tcc\, its licenses ...). A hub-only
 * apply never replaces these, even if a zip carried them. RetroHub's own files (RetroHub.exe, n64run.exe, data\, cores\, emu\ ...) are not. */
bool is_smash_classic_file(const std::wstring& rel);
/* hub_only: also drops Smash Classic's files (see above). */
std::vector<std::wstring> list_staged(const std::wstring& staged, bool hub_only = false);       // relative, backslashes, exclusions applied, sorted
std::string sha256_file_hex(const std::wstring& path);   // lowercase hex, "" when the file cannot be read
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
    std::string result;     // "failed" (rolled back) | "postponed" (a game was running) | "rollback_failed" (backup kept) | "interrupted" | "repaired" | "rolled_back" (the new RetroHub did not start; the old one is back)
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
    int pending = 0;      // new version running (or recorded bad) but its boot marker is missing: backup kept, nothing restored (see "boot check")
    int healthy = 0;     // an applier that was watching the new version's start died, and the new version is the one running: backup dropped, no note
    int restored_files = 0;
    std::vector<std::string> notes;   // one-line bell texts for the next start
};
RecoverReport recover_interrupted(const std::wstring& install, const std::string& current_version, const std::wstring& tmp_root = std::wstring());

/* ---- boot check: did the new RetroHub actually start? ----
 * The applier relaunches the new RetroHub with `--boot-ok "<marker file>" "<version>"`. Once that RetroHub has a window, a renderer and has
 * presented its first frame (or is up minimised behind a game) it calls note_boot_ok(), which writes <version> to the marker (temp +
 * rename, never fails the app). The applier, still alive and window-less, waits for the marker:
 *   marker with the expected version -> the update is good: the backup is deleted.
 *   the new process crashed (exit code >= 0xC0000000 except 0xC000013A) or exited with kStartFailExit (70), or could not start, without writing it -> ROLLBACK: the replaced files are restored
 *     from the backup, the files the update added are removed, the OLD RetroHub is relaunched quietly, a status note ("rolled_back",
 *     shown once in the bell) and updates\bad_version.json are written. Exit code 5.
 *   any other exit without the marker (0, a Task Manager kill = 1, logoff ...): kept, and so is the backup (exit 4). A downgrade (version < from) is never watched.
 *   the new process is still running after boot_wait_secs -> keep waiting (slow PC), never kill it; after boot_cap_secs stop watching and
 *     change nothing (backup kept for the next start to clean up). Exit code 4.
 * The applier never terminates any process and shows no UI. A rollback only starts when the new RetroHub is gone and nothing else runs
 * from the install folder (a game: it waits).
 *
 * updates\bad_version.json (written on a rollback; the updater must not offer or retry that version again):
 *   {"v":1,"version":"v1.2.3","from":"v1.2.2","at":<unix seconds>,"reason":"did not start","exit_code":<n>}
 * "version" is the release tag exactly as the update was offered. Read it with bad_version_read(); it stays until the updater sees a
 * DIFFERENT (newer) release, which it may try normally. */
std::wstring boot_ok_path(const std::wstring& install);      // <install>\updates\boot_ok
std::wstring bad_version_path(const std::wstring& install);  // <install>\updates\bad_version.json
struct BadVersion { std::string version, from, reason; long long at = 0; long exit_code = 0; };
bool bad_version_read(const std::wstring& install, BadVersion* out);   // false: none or unreadable
bool bad_version_write(const std::wstring& install, const BadVersion& b);
/* Removes updates\bad_version.json (the updater calls this once it installs a DIFFERENT, newer release). Safe when there is none. */
void bad_version_clear(const std::wstring& install);
/* RetroHub's exit code for "could not start" (SDL init / window / renderer failed). The applier rolls back ONLY on this code or a real
 * crash (exit code >= 0xC0000000, except 0xC000013A Ctrl-C/logoff). Any other exit without the marker (a Task Manager kill = 1, exit 0, ...)
 * keeps the update and the backup; the next start that wrote its marker cleans the backup up. */
constexpr int kStartFailExit = 70;
/* True when `v` is a lower dotted version than `than` (leading v ignored). False when either is unparsable. A downgrade is never watched. */
bool version_lower(const std::string& v, const std::string& than);
/* Called by the running RetroHub (main.cpp) once it is up. Cheap, idempotent, never throws. It ALWAYS records that this version started:
 * the marker goes to the --boot-ok path when the applier passed one, otherwise to <install>\updates\boot_ok, holding this RetroHub's own
 * version (set_own_version) and skipped when the file already says so. So a freshly updated hub that exits before it is watched to the end
 * (closed by the player, a second instance) still confirms itself on any later start, and startup recovery can drop the backup. */
void note_boot_ok();
void set_own_version(const std::string& version);   // this RetroHub's version, for note_boot_ok when no --boot-ok was given (updater.cpp passes it at startup)
/* Pure core of note_boot_ok (tested): makes <install>\updates\boot_ok hold `version`; no write when it already does. false when it could not. */
bool write_boot_marker(const std::wstring& install, const std::string& version);
/* Pure helper (tested): reads the marker and compares its version with `expect` (leading v and spaces ignored). */
bool boot_marker_matches(const std::wstring& marker, const std::string& expect);

/* ---- full flow ---- */
int run(const Options& o);

/* Parses the process command line; if it is `--apply-update ...` runs the applier and returns
 * true with the exit code. Call first thing in WinMain. */
bool try_apply_mode(int* exit_code);

} // namespace apply
} // namespace hubupdate
