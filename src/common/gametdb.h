#pragma once
/* GameTDB (https://www.gametdb.com): free community database + cover scans, used here for
 *   - full cover scans ("coverfull": BACK|SPINE|FRONT) and front covers of GameCube / Wii U / DS / 3DS games
 *     (art.gametdb.com/<wii|wiiu|ds|3ds>/<cover|coverHQ|coverfull|coverfullHQ>/<REGION>/<ID>.<png|jpg>),
 *   - the official database downloads (www.gametdb.com/<wiitdb|wiiutdb|dstdb|3dstdb|switchtdb>.zip, one XML each) as an ID /
 *     title / synopsis / developer / publisher / rating source (Wii U product codes are not in the game files we can read, so the
 *     title maps to its GameTDB id here; descriptions for games Wikipedia does not cover).
 * Pure logic + a small disk cache of compact per-system indexes; the network is a callback (no sockets here), like gameinfo.h.
 * No API key, no HTML scraping, nothing shipped with the app. Unit-tested by hub_art_test / hub_gameinfo_test. */

#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace gametdb {

/* Library system_short() -> GameTDB database name: gc -> "wii" (GameCube lives in wiitdb), wiiu, nds -> "ds", 3ds, switch. "" otherwise. */
std::string db_name(const std::string& system);
/* Systems that have a full cover scan on GameTDB (GameCube, Wii U, DS, 3DS). */
bool has_fullcover(const std::string& system);
/* Systems whose front cover can come from GameTDB when libretro has none (same four). */
inline bool has_cover(const std::string& system) { return has_fullcover(system); }

/* The GameTDB id of a library serial: gc / wiiu 6 chars ("gm4e01" -> "GM4E01"; a 4 char Wii U code gets "01"), nds / 3ds 4 chars.
 * "" when the serial is not that shape (a Wii U title id, a 3DS title id, ...). */
std::string id_from_serial(const std::string& system, const std::string& serial);

/* Region folders to try, best first, for a game: the folder implied by the id's region letter (E -> US, P -> EN, J -> JA, ...), then
 * an optional hint ("USA" / "Europe" / "Japan" / "World" from a file name), then US, EN, JA, then the rest. No duplicates. */
std::vector<std::string> region_folders(const std::string& id, const std::string& region_hint = "");
/* "Pokemon X (USA) (En,Ja)" -> "USA"; "Europe" / "Japan" / "World" / "Korea" ...; "" when no tag. */
std::string region_hint_from_name(const std::string& name);

/* The same game's other regional releases: ids that differ only in the region letter (index 3), original excluded, best first for `region_hint`
 * ("USA" / "Europe" / "Japan" / "Korea" / ""): the hint's region, then USA, Europe, Japan, Korea. Each pair is (id, scan folder). Pure. Used when
 * a game's own region has no full cover scan on GameTDB (Phantom Hourglass US: only the PAL scan exists; Pokemon X 3DS "EKJA" is filed as EKJE). */
std::vector<std::pair<std::string, std::string>> regional_siblings(const std::string& id, const std::string& region_hint);

enum class Img { Cover, CoverFull };
/* Candidate URLs for one region folder, best quality first (HQ, then standard). "" system/id -> empty. */
std::vector<std::string> image_urls(const std::string& system, const std::string& id, Img what, const std::string& region_folder);
std::string db_url(const std::string& system);   // https://www.gametdb.com/<name>.zip, "" when unsupported

/* ---- database records ---- */
struct Rec {
    std::string id, region;            // "ARDE01", "NTSC-U"
    std::string title, synopsis;       // English locale (else the first one)
    std::string developer, publisher, genre, esrb, pegi;
    int year = 0, month = 0, players = 0;
};
/* Parses a GameTDB XML file. type_filter ("GameCube") keeps only entries of that <type> ("" = all). Pure text scan, bounded. */
std::vector<Rec> parse_xml(const std::string& xml, const std::string& type_filter = "");
std::string to_tsv(const std::vector<Rec>& recs);
std::vector<Rec> parse_tsv(const std::string& tsv);

class Index {
public:
    void load(std::vector<Rec> recs);                 // everything in memory (tests, small inputs)
    /* Reads a TSV written by to_tsv(): only id / region / title (+ file offsets) stay in memory, the synopsis, developer, publisher ... of a
     * record are re-read from the file when asked for. False when the file is missing or empty. */
    bool load_file(const std::string& tsv_path);
    size_t size() const { return s_.size(); }
    std::optional<Rec> by_id(const std::string& id) const;
    /* Same normalized title (gameinfo::normalize of the title with ()/[] tags dropped); among several regional entries the best one:
     * USA first, then English PAL, then the rest; with want_synopsis an entry that has a synopsis beats one that has none. */
    std::optional<Rec> by_title(const std::string& title, bool want_synopsis = false) const;
private:
    struct Slim { std::string id, region, title; uint64_t off = 0; uint32_t len = 0; bool syn = false; };
    void add(Slim s);
    void finish();
    std::optional<Rec> fetch(size_t i) const;
    std::vector<Slim> s_;
    std::vector<Rec> mem_;                                   // load(): the records themselves (Slim::off indexes this)
    std::string path_;                                       // load_file(): where the records live
    std::vector<std::pair<std::string, size_t>> by_title_;   // sorted by normalized title
    std::vector<std::pair<std::string, size_t>> by_id_;
};

/* The record for a library game: by id (from the serial) first, then by title (any of `titles`, best regional entry); when the id's own
 * record has no synopsis a same-title record that has one is preferred. nullopt when unknown. */
std::optional<Rec> find_record(const Index& idx, const std::string& system, const std::string& serial, const std::vector<std::string>& titles);

/* Gets a URL: true when an HTTP response arrived (status set), false on a transport error. body gets the raw bytes. */
using Get = std::function<bool(const std::string& url, std::string* body, int* status)>;
/* Directory for the cached indexes ("<dir>/tdb_<db>.tsv"); "" = no database support. Call once at start. */
void set_dir(const std::string& dir);
std::string dir();
/* The system's index: from the cache (refreshed after 60 days), else downloaded once through `get`. nullptr when unavailable (offline,
 * no dir). A failed download is not retried for 5 minutes. Thread-safe; the returned pointer stays valid for the process. */
const Index* index_for(const std::string& system, const Get& get);
/* The index if it is already loaded or its file is on disk (read locally, never the network); nullptr otherwise. Safe on any thread. */
const Index* cached_index(const std::string& system);
/* Downloads + indexes in a background thread, at most once per database at a time (false = already loaded / already on its way / unsupported).
 * `done` runs on that thread once the index is usable. Never blocks the caller: this is the only way an automatic path may fetch a database. */
bool request_download(const std::string& system, const Get& get, std::function<void()> done);
/* First *.xml member of a zip held in memory ("" on failure; entries over 256 MB are refused). */
bool unzip_xml(const std::string& zip, std::string* xml);

} // namespace gametdb
