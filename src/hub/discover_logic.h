#pragma once
/* Discover page: curated "recommended games" per console (data/recommended.json, shipped next to RetroHub.exe in data\), plus a
 * browsable full catalog from data/gameinfo.tsv.
 * Metadata only: titles and facts, never a download link. Whether a game is already in the library, installable from a
 * shared GamePack, or has to be added by the player is decided by classify() (tests: test_discover.cpp). */

#include "friend_games.h"
#include "gamepack.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace gameinfo { class Db; }

namespace discover {

struct Rec {
    std::string title, sys, genre, why;
    std::string cid;     // games.h catalog id when the game is in the catalog ("" = not catalogued)
    std::string also;    // a second catalog id that counts as owning it (Smash 64: the ROM and the native port)
    int year = 0;
    int players = 1;     // most local players (catalog games: 0 = unknown)
    bool friends = false;   // "Great with friends"
    /* Filled by prepare() (curated) or catalog_from() / Browser::set_catalog (catalog games, which have no "why"). */
    std::string dev, pub;   // catalog games only
    int month = 0;          // catalog games only
    int score = 0;          // 0-100 from the Famitsu / Edge scores in gameinfo.tsv (catalog games; 0 = unrated)
    bool curated = true;
    int rank = 0;           // curated: position among its console's entries in file order
    int cidx = 0;           // index of the console in consoles()
    std::string key;        // hubgp::pack_key(title): the title identity used for matching and de-duplication
    std::string skey;       // sort key: folded lower-case title without a leading article
    std::string hay;        // folded lower-case search text (title; catalog games add developer and publisher)
};

/* Pure: parse the file text. Every game needs title, console, year, genre, players, why; a game that lacks one is dropped and
 * counted in *bad (optional). False when the text is not {"games":[...]}. */
bool parse(const std::string& text, std::vector<Rec>* out, int* bad = nullptr);

/* The shipped list (<exe dir>\data\recommended.json), read once. Empty when the file is missing. */
const std::vector<Rec>& all();

/* Console short codes in the order the picker shows them. */
const std::vector<std::string>& consoles();

/* Pure: the recommendations of one console in file order; friends_only keeps the "Great with friends" ones. */
std::vector<const Rec*> for_console(const std::vector<Rec>& recs, const std::string& sys, bool friends_only);

enum class Status { Owned, InPack, Missing };

struct Match {
    Status st = Status::Missing;
    std::string key;          // Owned: the library entry's key
    std::string pack_sys;     // InPack: the pack's console folder (differs from the rec's for Game Boy / Color twins)
    std::string pack_title;   // InPack: the title to hand to the GamePack installer
    std::string pack_hint;    // InPack: the Drive entry name without extension (exact art-name hint)
};

/* Pure: where a recommendation stands. A library game matches by catalog id first, then by title key (hubgp::pack_key, the same
 * rule the installer uses, GB / GBC twins included). A pack game matches by pack_pick on the console's pack index. */
Match classify(const Rec& r, const std::vector<hubui::fg::Owned>& owned, const std::vector<hubgp::PackGame>& pack);

/* Pure: "1 player" / "1-4 players". */
std::string players_text(int players);

/* ---- browsing: sort, filters, paging, the full catalog (all pure; the page only draws) ---- */

/* Lower-cases ASCII and folds Latin-1 accents to plain letters (e-acute -> e) so searches match without typing accents. */
std::string fold_text(const std::string& s);

/* Fill rank / cidx / key / skey / hay on every curated entry (file order = recommended rank, per console). */
void prepare(std::vector<Rec>* recs);

/* gameinfo.tsv genre ("Role-playing (RPG)", "Shoot'em Up", "Sports / Basketball") -> one of the curated genre names where it
 * fits (RPG, Platformer, Shooter, ...), else a tidy bucket ("Board & Card", "Compilation", "Other"). */
std::string genre_bucket(const std::string& g);

/* Consoles gameinfo.tsv has metadata for (the others have no "full catalog"). */
const std::vector<std::string>& catalog_consoles();
bool catalog_covers(const std::string& sys);

/* Catalog games (every game of catalog_consoles() in the database) that are not already in `curated` (same console, GB / GBC
 * twins included, same title key) and not repeated within the catalog. */
std::vector<Rec> catalog_from(const gameinfo::Db& db, const std::vector<Rec>& curated);

enum class Sort { Recommended, Title, Newest, Oldest, Players };
const char* sort_key(Sort s);                      // "rec" "title" "new" "old" "players" (the saved setting)
Sort sort_from_key(const std::string& k);          // unknown -> Recommended
const char* sort_label(Sort s);                    // "Recommended" "Title A-Z" "Newest" "Oldest" "Most players"
inline constexpr int kSortCount = 5;

enum class StatusFilter { Any, NotOwned, InPack, Owned };   // Not in my library (includes pack games) / Installable from a shared pack / In my library

struct Filter {
    std::string sys;                // "" = all consoles
    bool catalog = false;           // false: the curated list; true: the full catalog (games not in the curated list)
    std::set<std::string> genres;   // empty = any; a game passes when its genre is one of these
    int min_players = 0;            // 0 any, 2, 3, 4
    int decade = 0;                 // 0 any, else 1980 / 1990 / 2000 / 2010
    bool friends = false;           // "Great with friends" (curated games only; ignored in the catalog)
    StatusFilter status = StatusFilter::Any;
    std::string query;              // words, all must appear in the title (catalog games: or developer / publisher)
    Sort sort = Sort::Recommended;
};
/* True when anything but console / catalog / sort narrows the list (drives "Clear filters"). */
bool filters_active(const Filter& f);
void clear_filters(Filter* f);      // keeps console / catalog / sort
std::string filter_sig(const Filter& f);

/* Paging: the first kBatch, then kBatch more each time. */
inline constexpr size_t kBatch = 24;
struct Pager {
    size_t shown = kBatch;
    void reset() { shown = kBatch; }
    size_t visible(size_t total) const { return shown < total ? shown : total; }
    bool has_more(size_t total) const { return shown < total; }
    void more(size_t total) { if (shown < total) shown += kBatch; }
};
std::string showing_text(size_t visible, size_t total);   // "Showing 48 of 103"

/* Prebuilt matcher for many classify() calls: same answers as classify(), but a word index narrows the loose-title search so a
 * whole 12,000-game catalog can be checked without a hitch. */
class Classifier {
public:
    Classifier() = default;
    Classifier(std::vector<hubui::fg::Owned> owned, std::vector<hubgp::PackGame> pack);
    Match classify(const Rec& r) const;
private:
    struct Group {   // the owned list and the pack list of one console (GB / GBC twins together), in classify()'s order
        std::vector<std::string> okeys, pkeys;
        std::vector<size_t> osrc, psrc;
        std::unordered_map<std::string, std::vector<int>> owords, pwords;
    };
    const Group& group(const std::string& sys) const;
    std::vector<hubui::fg::Owned> owned_;
    std::vector<hubgp::PackGame> pack_;
    mutable std::map<std::string, Group> groups_;
};

/* The browsable set: curated entries first (indices 0..curated_count()-1), then the catalog games. Status (library / pack) is
 * worked out per game on demand and remembered. */
class Browser {
public:
    void set_curated(std::vector<Rec> recs);                 // prepares them; drops the catalog
    void set_catalog(std::vector<Rec> cat);                  // from catalog_from()
    void set_sources(std::vector<hubui::fg::Owned> owned, std::vector<hubgp::PackGame> pack);   // library / pack changed: forget statuses
    bool has_catalog() const { return items_.size() > ncur_; }
    size_t curated_count() const { return ncur_; }
    size_t size() const { return items_.size(); }
    const Rec& item(int i) const { return items_[(size_t)i]; }
    const Match& match(int i);                                // classified on first use

    struct Result {
        std::vector<int> ids;                  // matching games in sort order
        std::map<std::string, int> by_console; // matches per console when the console filter is ignored (the chip counts)
        int total_all = 0;                     // same, all consoles
        int pending = 0;                       // games whose status is still unknown (status filter): ask again next frame
    };
    /* budget_ms < 0: classify everything needed now; otherwise stop classifying after about that long and report `pending`. */
    Result query(const Filter& f, double budget_ms = -1.0);
    /* Genres in the current universe with counts, filtered by everything except the genre filter itself (and status). Sorted by name. */
    std::vector<std::pair<std::string, int>> genre_counts(const Filter& f) const;
    size_t universe_size(bool catalog, const std::string& sys) const;   // games available before any filter

private:
    bool passes(const Rec& r, const Filter& f, const std::vector<std::string>& words, bool skip_genre) const;
    std::vector<Rec> items_;
    size_t ncur_ = 0;
    Classifier cls_;
    std::vector<Match> memo_;
    std::vector<char> known_;
};

}  // namespace discover
