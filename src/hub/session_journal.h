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
 *   id, kind (hubemu::Kind as int), exe (emulator exe path, "" = unknown), system, game, title, pid, created (process creation time,
 *   FILETIME ticks), attempts, active (0/1), emu_root, battery, backup_dir, manifest, rel (repeated), vrel (repeated, parallel to rel),
 *   wiiu_unknown (0/1), wiiu_parent, started (unix s), own_install (0/1), snapshot_emu, psp_disc, nx_dir, nx_tid (hex), cemu_user,
 *   cemu_tid (hex), revert_profile (0/1), revert_settings (0/1).
 * A file that does not parse is renamed to <name>.corrupt and never trusted. */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "emulators.h"

namespace hubjournal {

struct Entry {
    std::string id;                  // file stem; assigned by write()
    int emu_kind = 0;                // hubemu::Kind
    std::string emu_exe;             // the emulator's exe path (a running emulator of this name defers the replay)
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
/* The after-exit work: reverts, the Switch save mapping re-evaluated now, then hubemu::end_session. Used by the normal after_exit and by
 * the replay, so both behave the same. `s` is the live session (normal) or meta.sess (replay). */
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
bool process_running(unsigned long pid, unsigned long long created);

struct RecoverReport {
    int replayed = 0, still_running = 0, deferred = 0, corrupt = 0, failed = 0;
    std::vector<std::string> notes;
};
/* Replays every journal in `dir` whose emulator has ended. `after` runs after a successful replay (vault bookkeeping, cloud push). */
RecoverReport recover(const std::string& dir, const std::function<void(const Entry&)>& after);

/* Background recovery for the hub: one pass now, then every ~15 s while something is still waiting for its emulator to end. */
void start_recovery(const std::string& dir, std::function<void(const Entry&)> after, std::function<void(const std::string&)> log);
void stop_recovery();

} // namespace hubjournal
