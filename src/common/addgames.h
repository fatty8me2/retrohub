#pragma once
/* "Add games": copy the player's own game files into RetroHub's games folder, sorted into a console subfolder.
 * Pure logic + a worker-thread copy job; no UI. Files are only ever COPIED (never moved or deleted). */

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "library.h"

namespace hub::addgames {

/* Extensions the picker offers (lowercase, with dot): every format the library can identify, plus .zip/.7z. */
const std::vector<std::string>& supported_exts();
bool is_game_file(const std::string& path);   // by extension only

/* Console subfolder under the games root; matches the installer's folders. Unknown -> "Other". */
std::string console_folder(System s);

/* The games root: the first rom folder named "RetroHub Games" (the installer's choice), else <documents>\RetroHub Games. */
std::string games_root(const std::vector<std::string>& rom_folders, const std::string& documents_dir, const std::string& custom_root = "");
/* custom_root (the player's chosen destination, persisted in settings) wins when it is an absolute path. */
bool is_absolute_path(const std::string& p);

/* Fresh index of the game keys already under `root` (every supported file is identified/hashed now), so duplicates are caught
 * even when the library scan is unfinished or stale. Returns early (partial) when *stop becomes true. */
std::set<std::string> index_keys(const std::string& root, const std::atomic<bool>* stop = nullptr);
std::string dest_dir(const std::string& root, System s);   // root\<console>

/* Destination file name that does not clash: "name.ext", then "name (2).ext", ... `exists` tests a full path. */
std::string unique_name(const std::string& dir, const std::string& file_name, const std::function<bool(const std::string&)>& exists);

enum class Outcome { Added, Duplicate, Unsupported, Failed, Cancelled };
struct Item {
    std::string src, title, dest, message;
    System system = System::Unknown;
    Outcome outcome = Outcome::Failed;
    bool other = false;   // unidentified: went to "Other"
};

/* "Added 3 games: A, B, C" / "Already in your library: X" / warnings, as one short multi-line text. */
std::string summary_text(const std::vector<Item>& items);
int count_outcome(const std::vector<Item>& items, Outcome o);

/* Chunked copy to dst via dst+".part" then rename. progress(done,total) returns false to cancel. */
bool copy_file(const std::string& src, const std::string& dst, uint64_t* copied_total,
               const std::function<bool(uint64_t, uint64_t)>& progress, std::string* err);

/* Runs on a worker thread. known_keys: GameEntry::key of everything already in the library. */
class Job {
public:
    ~Job() { join(); }
    void start(std::vector<std::string> files, std::string root, std::set<std::string> known_keys);
    void cancel() { cancel_ = true; }
    bool done() const { return done_; }
    void join() { if (th_.joinable()) th_.join(); }
    /* progress snapshot (any thread) */
    uint64_t bytes_done() const { return bytes_done_; }
    uint64_t bytes_total() const { return bytes_total_; }
    int file_index() const { return file_idx_; }
    int file_count() const { return file_cnt_; }
    std::string current_name();
    std::vector<Item> items();   // valid once done()
private:
    void run(std::vector<std::string> files, std::string root, std::set<std::string> known_keys);
    std::thread th_;
    std::atomic<bool> done_{false}, cancel_{false};
    std::atomic<uint64_t> bytes_done_{0}, bytes_total_{0};
    std::atomic<int> file_idx_{0}, file_cnt_{0};
    std::mutex mu_;
    std::string cur_;
    std::vector<Item> items_;
};

}  // namespace hub::addgames
