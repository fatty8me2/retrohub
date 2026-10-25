#pragma once
/* Local save history (Wave 2 G): the last few versions of a game's battery/ folder, kept on this PC.
 *   <hist_dir>/<id>/<relative files...> + .info     one snapshot per play session that changed the save (id = zero padded unix time)
 *   <hist_dir>/_undo/...                            the files as they were just before the latest restore
 * Only small files are kept (default <= 512 KB each, so memory cards of 8 MB do not pile up). Pure file logic on std::filesystem,
 * every function takes the directories explicitly so tests need no vault. */

#include <cstdint>
#include <string>
#include <vector>

namespace hub::saves {

struct HistEntry {
    std::string id;         // folder name; also what restore takes
    int64_t ts = 0;         // unix seconds when the snapshot was taken
    int64_t size = 0;       // bytes of all files
    int files = 0;
    std::string device;
};

constexpr int kHistKeepLocal = 5;
constexpr int64_t kHistMaxFile = 512 * 1024;

/* Copies battery_dir (files <= max_file) into a new snapshot unless it matches the newest one exactly (or is empty).
 * Prunes to the newest `keep`. Returns true when a snapshot was made. */
bool history_snapshot(const std::string& hist_dir, const std::string& battery_dir, int64_t ts, const std::string& device,
                      int keep = kHistKeepLocal, int64_t max_file = kHistMaxFile);
std::vector<HistEntry> history_list(const std::string& hist_dir);            // newest first, never includes the undo copy
/* Puts snapshot `id` back into battery_dir. The current files are first saved as the undo copy (replacing an older one). */
bool history_restore(const std::string& hist_dir, const std::string& battery_dir, const std::string& id, int64_t ts, std::string* err);
/* The undo copy taken by the latest restore (ts == 0 when there is none). */
bool history_undo_info(const std::string& hist_dir, HistEntry* out);
/* Puts the undo copy back (the files it holds are written over battery_dir) and drops it. */
bool history_undo(const std::string& hist_dir, const std::string& battery_dir, std::string* err);
/* Saves the current battery_dir as the undo copy (used before a cloud restore, which writes the files itself). */
bool history_mark_undo(const std::string& hist_dir, const std::string& battery_dir, int64_t ts, const std::string& device,
                       int64_t max_file = kHistMaxFile);

/* ---- unified view: local snapshots and cloud versions in one list ---- */
struct CloudVer { std::string path; int ver = 0; int64_t ts = 0, size = 0; std::string device; bool current = false; };
struct HistRow {
    bool cloud = false;
    std::string id;         // local: snapshot id; cloud: ""
    std::string path;       // cloud: file path in the game ("battery/x.sra"); local: ""
    int ver = 0;
    int64_t ts = 0, size = 0;
    std::string device;
    bool current = false;   // the cloud version that is live now (nothing to restore)
    int files = 0;
};
/* Newest first; ties put local before cloud. */
std::vector<HistRow> history_merge(const std::vector<HistEntry>& local, const std::vector<CloudVer>& cloud);

/* Zips one game's folder (battery/, optionally states/, plus meta.json) so it can be shared or backed up. */
bool export_game_zip(const std::string& game_dir, const std::string& zip_path, bool include_states, std::string* err);

/* Convenience for the hub: the history folder of a vault game (<data_dir>/save_history/<system>/<key>/). */
std::string game_history_dir(const std::string& system_short, const std::string& game_key);
/* Session-end hook: snapshots the game's battery folder. */
bool history_snapshot_game(const std::string& system_short, const std::string& game_key);

} // namespace hub::saves
