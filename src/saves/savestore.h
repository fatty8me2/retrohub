#pragma once
/* Save vault: every game's save data lives under one portable folder, and can be
 * synced two-way with a user-chosen folder (OneDrive/Dropbox/USB/NAS) or
 * exported/imported as one zip -- so saves follow the player across PCs and OS
 * reinstalls.
 *
 * Layout:  <vault>/<system short>/<game key>/
 *              battery/   in-game saves (SRAM/EEPROM/flash, GC memory-card GCI files, ...)
 *              states/    save states (slot0..9 + auto)
 *              meta.json  {key, title, system, updated (unix s), device (machine name)}
 * <vault> defaults to hub::data_dir() + "saves\\". */

#include <cstdint>
#include <string>
#include <vector>

namespace hub::saves {

std::string vault_dir();
void set_vault_dir(const std::string& dir);  // advanced setting; default is data_dir()/saves

/* Directories for one game (created on demand). Runners are pointed at these. */
std::string game_dir(const std::string& system_short, const std::string& game_key);
std::string battery_dir(const std::string& system_short, const std::string& game_key);
std::string states_dir(const std::string& system_short, const std::string& game_key);
/* Call after a play session: refreshes meta.json (title, updated, device). */
void touch_game(const std::string& system_short, const std::string& game_key, const std::string& title);

struct SyncReport {
    int pushed = 0;      // files copied vault -> sync folder
    int pulled = 0;      // files copied sync folder -> vault
    int conflicts = 0;   // both sides changed since last sync: newer wins, loser kept in _conflicts/
    std::vector<std::string> log;
    bool ok = true;
    std::string error;
};

/* Two-way sync of the whole vault with `sync_root` (the same layout is mirrored
 * there). Tracks a per-file manifest (size, mtime, SHA-256) from the last sync
 * in <vault>/.sync/<sanitized sync_root>.json to tell "changed here" from
 * "changed there". Never deletes the only copy of anything; deletions are not
 * propagated (v1). Skips states/ unless include_states. */
SyncReport sync_folder(const std::string& sync_root, bool include_states);

/* Whole-vault backup/restore. Import merges: newer file wins, older kept in _conflicts/. */
bool export_zip(const std::string& zip_path, bool include_states, std::string* error);
SyncReport import_zip(const std::string& zip_path);

} // namespace hub::saves
