#pragma once
/* What the LAST scan_library() learned beyond its game list (library-1 and library-3). Kept out of library.h on purpose (Opus owns it);
 * fold these declarations into library.h when convenient. Thread-safe; the result is replaced by every scan. */
#include "library.h"

#include <set>
#include <string>
#include <vector>

namespace hub {

/* library-1: every other copy of a game the scan found (same key, different file). The list returned by scan_library() holds one copy per key
 * (the preferred one); these are the rest, so the library can offer "N copies found - use this one". */
std::vector<GameEntry> scan_extra_copies();

/* library-3: one row per folder that was passed to scan_library().
 *   reachable = false  the folder could not be opened (drive or share not connected)
 *   complete  = false  it could not be read to the end (unreachable, or a directory failed to list)
 *   kept      = games of that folder still listed from the last good scan (they sit in the returned list with their old paths) */
struct FolderScanStatus {
    std::string folder;
    bool reachable = true;
    bool complete = true;
    int kept = 0;
};
std::vector<FolderScanStatus> scan_folder_status();

/* The folder the RUNNING scan is reading right now (read-only progress for the hub's caption); "" when no scan runs. Thread-safe. */
std::string scan_current_folder();

/* library-2: games removed from THIS PC's library only (never synced; "Remove from this PC" on the missing-file popup).
 * Stored in <library dir>/removed_here.txt, one game key per line. Hiding a game on every PC stays a separate choice (prefs `hidden`). */
std::set<std::string> local_removed();
bool local_removed_set(const std::string& key, bool removed);   // false if the file could not be written

/* library-13: the game keys of `paths` that the scan cache (path + size + mtime) already knows, so "Add games" does not re-hash the whole
 * destination. Paths the cache cannot answer (new, changed, never scanned) are appended to *uncached for the caller to identify. */
std::set<std::string> cached_keys_for_files(const std::vector<std::string>& paths, std::vector<std::string>* uncached);

}  // namespace hub
