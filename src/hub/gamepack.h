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

/* Pure: the key a friend's game title and a pack folder name are matched on: norm_title plus "version"/"edition"
 * dropped and e-acute folded, so "Pokemon Platinum" matches the folder "Pokemon - Platinum Version (USA)". */
std::string pack_key(const std::string& title);

/* Pure: index of the pack entry key in `keys` that a friend's game key `want` names: an exact key first, else the one loose
 * match (same words in any order, or one side's words inside the other's with no extra number but 64). -1 when none, or when
 * two different entries match loosely (never guess between them). */
int pack_pick(const std::vector<std::string>& keys, const std::string& want);

/* Pure: a title made safe for use as a folder name (Windows-illegal characters become spaces, runs collapsed). */
std::string safe_name(const std::string& t);

/* Pure: Drive folder ids from pasted text - drive.google.com/drive/folders/<id>, /folder/<id> (old form),
 * embeddedfolderview?id=<id>, or bare ids. Order preserved, duplicates dropped. */
std::vector<std::string> parse_links(const std::string& text);

/* Pure: the stored pack_links string <-> a list of canonical folder ids. Reading accepts the old comma / space /
 * newline separated text (ids or full URLs); writing is space-separated ids. */
std::vector<std::string> links_split(const std::string& stored);
std::string links_join(const std::vector<std::string>& ids);
/* Add every folder id found in `pasted` to `stored` (links_add). Returns how many were new; *found (optional) = how many
 * valid ids the text held (0 = nothing usable). Result is re-canonicalised. */
int links_add(std::string* stored, const std::string& pasted, int* found = nullptr);
/* Remove one id; false when it was not there. */
bool links_remove(std::string* stored, const std::string& id);
/* Pure: a short readable label for a folder id ("1abcDEFg...wxyz" for long ids). */
std::string link_label(const std::string& id);

/* Pure: the shared community pack list (data/packs.json on GitHub, fetched by every client so a link added there
 * is installable for everyone without per-user setup). Accepts {"packs":[{"id":"...","name":"..."}]} or a bare
 * array of ids / objects. Returns the valid, de-duplicated folder ids (bad ids dropped); false when not that JSON. */
bool parse_packs_json(const std::string& text, std::vector<std::string>* out);

/* Pure: the console short code a pack subfolder name means ("Wii u" -> wiiu, "GameCube" -> gc, "SNES" -> snes,
 * "PS2" -> ps2 ...), or "" when it names no console. */
std::string console_alias(const std::string& name);

/* Pure: true when `name` is a console short code usable as a pack subfolder (n64, gc, wiiu, 3ds, nds, gba, gbc, gb, ps1). */
bool console_code(const std::string& name);

/* List a Drive folder. Returns false on network error (out is left empty). */
bool list_folder(const std::string& folder_id, std::vector<Entry>* out);

/* The game subfolder of `sys_short` whose name matches `title` (norm_title on both sides), in the built-in pack or
 * in one of `roots` (extra shared folders with the same layout: a subfolder per console). "" when absent. */
std::string match_game(const std::string& sys_short, const std::string& title, const std::vector<std::string>& roots = {});

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

/* Background index of what is installable: the built-in pack, the shared community packs (fetched from GitHub)
 * and every extra root (same layout). One job at a time; index_refresh() is a no-op while one runs. Lets the UI
 * grey out Install for games that are not in any pack without a network round-trip per click. */
enum class IndexState { Unknown = 0, Checking = 1, Ready = 2 };
void index_refresh(const std::vector<std::string>& roots);   // no-op while a job runs
void index_shutdown();                                       // app exit: stop the job and join it
long long index_last_attempt_ms();                           // 0 = never attempted (steady clock, ms)
IndexState index_state(const std::string& sys_short);        // per console; Ready = membership known for it
bool index_has(const std::string& sys_short, const std::string& title);   // false unless Ready
bool index_source(const std::string& sys_short);             // a pack source exists for this console (built-in, or an extra root once known)

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
    void start(const std::string& sys, const std::string& title, const std::vector<std::string>& roots = {});   // state -> Working
    void request_cancel();                                          // stops between files / mid-transfer
    /* UI thread, each frame: when the job ended, join it and return true exactly once; success tells the result. */
    bool poll(bool* success);
};

} // namespace hubgp
