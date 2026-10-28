#pragma once
/* GamePack: a friend's greyed-out game can be installed from the shared Google Drive folder (GamePack1, public
 * link). The game's files are downloaded into RetroHub's managed games folder (<data dir>\gamepack\<console>\<title>)
 * and that folder joins the library. UI-free: friend_games_ui.cpp draws the button + progress dialog. */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hubgp {

/* The shared Drive folder (public, anyone with the link). */
const char* root_folder();

/* The Drive folder id for a system short code ("" = that console is not in the pack). */
const char* sys_folder(const std::string& sys_short);

/* One entry in a Drive folder listing. */
struct Entry { std::string id, name; bool is_folder = false; };

/* Pure: decode HTML entities (&amp; &lt; &gt; &quot; &apos; &#NNN; &#xHH;). */
std::string decode_entities(const std::string& s);

/* Pure: parse the embeddedfolderview HTML of a Drive folder into entries (order preserved). */
std::vector<Entry> parse_folder_html(const std::string& html);

/* Pure: a title made safe for use as a folder name (Windows-illegal characters become spaces, runs collapsed). */
std::string safe_name(const std::string& t);

/* List a Drive folder. Returns false on network error (out is left empty). */
bool list_folder(const std::string& folder_id, std::vector<Entry>* out);

/* The game subfolder of `sys_short` whose name matches `title` (norm_title on both sides). "" when absent. */
std::string match_game(const std::string& sys_short, const std::string& title);

/* The files inside a game folder (subfolders skipped). */
bool list_files(const std::string& game_folder_id, std::vector<Entry>* out);

/* Download one Drive file to `out_path` (overwriting). got/total track the current transfer; stop may abort it. */
bool download_file(const std::string& file_id, const std::string& out_path,
                   std::atomic<uint64_t>* got, std::atomic<uint64_t>* total,
                   std::atomic<bool>* stop, std::string* err);

/* RetroHub-managed games folder: <data dir>\gamepack\ */
std::string pack_root();

/* True when the game is already installed under pack_root() (its folder holds the gamepack.done marker,
 * written only when every file finished: a cancelled / partial install can be retried). */
bool installed(const std::string& sys_short, const std::string& title);

/* Background install job (one at a time). */
struct Installer {
    enum State { Idle, Working, Failed } state = Idle;
    std::string sys, title;                 // what is being installed (valid until the next start)
    std::atomic<bool> done{false}, ok{false};
    std::atomic<bool> cancel{false};        // request_cancel(): stops between files / mid-transfer
    std::atomic<uint64_t> got{0}, total{0}; // bytes of the current file
    std::atomic<int> file_idx{0}, file_count{0};
    std::string current;                    // name of the file being downloaded (mu)
    std::string error;                      // mu ("cancelled" when the user stopped it)
    std::mutex mu;
    std::thread th;
    ~Installer();
    void start(const std::string& sys, const std::string& title);   // state -> Working
    void request_cancel();                                          // stops between files / mid-transfer
    /* UI thread, each frame: when the job ended, join it and return true exactly once; success tells the result. */
    bool poll(bool* success);
};

} // namespace hubgp
