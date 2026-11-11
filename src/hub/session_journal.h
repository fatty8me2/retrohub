#pragma once
/* Pending-session journal for external emulators (Dolphin, Azahar, Cemu, PCSX2, PPSSPP, Lichii, Fritchii).
 *
 * The after-exit work of a session (copy the emulator's saves into the vault, put back the temporary settings RetroHub changed in a
 * player's own Cemu) normally runs when the hub sees the emulator process end. If the hub is closed while the emulator still runs, that
 * never happens. So at launch a small journal file records what that work needs; it is deleted when the work is done. At the next hub
 * start (and later while the hub runs) any journal whose emulator has ended is replayed through the same code, exactly once.
 *
 * Location: <hub data dir>\pending_sessions\<id>.session   (UTF-8 text, one "key<TAB>value" per line, written atomically)
 * Format (version 1): first line "RHJ1", last line "end". Values escape backslash, tab, newline, return as \\ \t \n \r. Keys:
 *   id, kind (hubemu::Kind as int, 100 = Hookshot), exe (emulator exe path, "" = unknown), inst (install folder), inst_managed (0/1), system, game, title, pid, created (process creation time,
 *   FILETIME ticks), attempts, active (0/1), emu_root, battery, backup_dir, manifest, rel (repeated), vrel (repeated, parallel to rel),
 *   wiiu_unknown (0/1), wiiu_parent, started (unix s), own_install (0/1), snapshot_emu, psp_disc, nx_dir, nx_tid (hex), cemu_user,
 *   cemu_tid (hex), revert_profile (0/1), revert_settings (0/1).
 * A file that does not parse is renamed to <name>.corrupt and never trusted. */

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "emulators.h"

namespace hubjournal {

struct Entry {
    std::string id;                  // file stem; assigned by write()
    int emu_kind = 0;                // hubemu::Kind
    std::string emu_exe;             // the emulator's exe path (a process running from exactly this image defers the replay)
    std::string inst_dir;            // the install folder at launch (identity: the replay re-derives every path from the CURRENT install and refuses on a mismatch)
    bool inst_managed = false;       // RetroHub's own download: ANY process running from inst_dir counts as the emulator still running
    std::string game_path;           // the game file the launch passed to hubemu::find (Cemu may be found next to it); "" in older journals
    std::string system_short, game_key, title;   // for the vault bookkeeping after a replay
    hubemu::SaveSession sess;        // as begin_session left it
    std::string nx_install_dir;      // Switch: install whose save mapping is re-evaluated at exit
    uint64_t nx_title_id = 0;
    std::string cemu_user;           // Wii U: Cemu user data folder
    uint64_t cemu_tid = 0;
    bool revert_profile = false;     // put back the per-game Cemu profile RetroHub wrote
    bool revert_own_settings = false;   // put back the player's own Cemu settings.xml
    unsigned long pid = 0;           // emulator process (0 = not started yet)
    unsigned long long created = 0;  // its creation time, so a recycled pid is not mistaken for it
    int attempts = 0;                // failed replays so far
};

std::string pending_dir(const std::string& data_dir);   // "<data dir>\pending_sessions\"
std::string serialize(const Entry& e);
bool parse(const std::string& text, Entry* out);        // false = corrupt (nothing in *out is to be trusted)

/* Writes (or rewrites) the journal atomically and registers the session as live in this process. Assigns e.id when empty. */
bool begin(const std::string& dir, Entry& e);
/* Rewrites an already begun journal (the process id became known). */
bool update(const std::string& dir, const Entry& e);
/* ONE lock shared by launching and replaying. A launch holds it from before begin_session until the spawn returned and the journal is written;
 * recover() takes it per journal, re-checks everything inside, and keeps it for the whole replay of that journal. The UI thread only try_locks it
 * (a replay copies a save tree: never freeze a frame) and tells the player to try again. */
std::mutex& session_lock();

/* The after-exit work: reverts, the Switch save mapping re-read now (only when it names the same user as at launch), then hubemu::end_session.
 * Used by the normal after_exit and by the replay, so both behave the same. `s` is the live session (normal) or the re-derived one (replay). */
void run_exit(hubemu::SaveSession& s, const Entry& meta);
/* Normal end of a session: run_exit, then delete the journal when the copy-back had no error (an error keeps it for a retry at the next start). */
void finish(hubemu::SaveSession& s, const Entry& meta, const std::string& dir);
/* The emulator never started: put the temporary settings back and delete the journal. Saves are untouched (nothing ran). */
void abort_unspawned(const Entry& meta, const std::string& dir, bool revert);

/* The Switch save folders as they exist NOW added to a session's list (a folder the emulator created during play); missing ones stay a no-op. */
bool merge_nx_map(hubemu::SaveSession& s, const hubemu::NxSaveMap& m);

/* Forgets this process's live sessions (what a hub shutdown does implicitly; tests use it to simulate "the hub was closed"). */
void forget_live();

/* True when that process (same id AND creation time) is still running. A pid that is not there, or now belongs to another process,
 * is not running. A process that cannot be inspected counts as running. */
bool process_running(unsigned long pid, unsigned long long created, const std::string& exe = std::string(), long long started_unix = 0);
/* With created == 0 (no creation time recorded): the process image is compared with `exe` when that is known and the pid counts as another
 * program when it differs; with nothing to compare, the pid counts as recycled only once the journal (started_unix) is over 24 hours old. */
/* True when any process runs from exactly the image `exe` (full path, case-insensitive), or from inside `managed_dir` when that is not empty.
 * An unrelated program with the same exe NAME elsewhere does not count. */
bool image_running(const std::string& exe, const std::string& managed_dir);

/* What the replay may trust: the CURRENT configuration. The journal only names identity (emulator kind, game key, pid, which reverts are pending);
 * every folder is derived again from this, and a journal that disagrees with it is kept (3 tries, then renamed *.stale), never silently dropped. */
struct Config {
    std::function<std::string(const std::string& system_short, const std::string& game_key)> battery_dir;   // the vault battery dir NOW (must not create folders)
    std::function<bool(const Entry&, std::string* install_dir, bool* managed)> install;                       // the emulator install NOW
    std::string appdata;                                                                                      // %APPDATA% (own-Cemu user data)
};

struct RecoverReport {
    int replayed = 0, still_running = 0, deferred = 0, corrupt = 0, failed = 0;
    std::vector<std::string> notes;      // information
    std::vector<std::string> warnings;   // a journal that was kept or set aside, and why
};
/* Replays every journal in `dir` whose emulator has ended. `after` runs after a successful replay (vault bookkeeping, cloud push). `stop` is
 * checked between journals and between files. */
RecoverReport recover(const std::string& dir, const Config& cfg, const std::function<void(const Entry&)>& after, const std::atomic<bool>* stop = nullptr);
/* Test seam: called inside the lock right before a replay copies anything. */
void set_replay_hook(std::function<void()> hook);
/* Where run_exit reports a mapping it did not trust (set by start_recovery; unset = silent). */
void set_log(std::function<void(bool warning, const std::string&)> log);

/* Background recovery for the hub: one pass now, then every ~15 s while something is still waiting for its emulator to end. */
void start_recovery(const std::string& dir, Config cfg, std::function<void(const Entry&)> after, std::function<void(bool warning, const std::string&)> log);
/* Stops and joins the thread (prompt: it checks a flag between files). Safe to call twice and when never started. */
void stop_recovery();

} // namespace hubjournal
