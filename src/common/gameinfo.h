#pragma once
/* Game info for the library details modal: pure logic, no UI, no sockets (the network is a callback).
 *
 *  - Offline metadata: data/gameinfo.tsv, built by scripts/gameinfo_build.js from libretro-database metadat/
 *    (CC BY-SA 4.0; see THIRD_PARTY.md): developer, publisher, genre, release year/month, players, ESRB, Famitsu/Edge scores.
 *  - Online extras (description, review scores, age ratings): Wikidata + Wikipedia REST summary, no API key, resolved on demand
 *    by resolve_online() through a Fetcher and cached per game by the caller (cache_to_json / cache_from_json).
 *    A match needs BOTH the title (label/alias) and the platform to agree; otherwise nothing from the web is shown. */

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gametdb { struct Rec; }

namespace gameinfo {

/* "Pokémon: Emerald Version", "Pokemon - Emerald" -> "pokemonemerald": lowercase ASCII, accents folded, "&" -> "and",
 * the word "version" and a leading "the" dropped, everything but [a-z0-9] removed. */
std::string normalize(const std::string& title);
/* "SLUS_200.62" / "slus-20062" -> "SLUS-20062". Other serials: uppercased. */
std::string normalize_serial(const std::string& s);
/* "Legend of Zelda, The (USA) (Rev A)" -> "The Legend of Zelda"; tags in () [] are dropped. */
std::string clean_title(const std::string& s);

struct Query {
    std::string system;   // library system_short(): n64 gba gb gbc gc nds 3ds wiiu ps1 ps2
    std::string title;    // what the library shows
    std::string alt_title;   // curated catalog title, tried first when set
    std::string serial;   // GameEntry::serial (header ID / disc ID / boot serial)
};

/* ---- offline metadata ---- */
struct Meta {
    bool found = false;
    std::string title, developer, publisher, genre, esrb;
    int year = 0, month = 0;
    int players = 0;      // max users, 0 = unknown
    int famitsu = 0;      // out of 40, 0 = none
    int edge = 0;         // out of 10, 0 = none
    bool by_serial = false;
};

class Db {
public:
    /* Parse gameinfo.tsv text (see gameinfo_build.js). Returns the number of games loaded. */
    size_t load(const std::string& tsv);
    bool loaded() const { return !rows_.empty(); }
    size_t size() const { return rows_.size(); }
    /* Serial first (exact), then the normalized title of alt_title / title. Never fuzzy. */
    Meta lookup(const Query& q) const;
    struct Row { std::string sys, title, dev, pub, genre, esrb; int year = 0, month = 0, players = 0, famitsu = 0, edge = 0; };
    /* Every game in file order (the Discover page's full-catalog browser reads these). */
    const std::vector<Row>& rows() const { return rows_; }
private:
    std::vector<Row> rows_;
    std::map<std::string, size_t> by_serial_;   // "sys|SERIAL"
    std::map<std::string, size_t> by_title_;    // "sys|normalized"
    Meta to_meta(const Row& r, bool by_serial) const;
};

/* Shared database loaded from <dir>/gameinfo.tsv on first use (thread-safe). Empty when the file is missing. */
const Db& shared_db(const std::string& dir);

/* ---- online ---- */
struct Rating { std::string source, text; };   // {"Metacritic", "92"}, {"Famitsu", "37/40"}

struct Online {
    enum Status { Unknown, Ok, NotFound };
    Status status = Unknown;
    int64_t fetched = 0;               // unix seconds
    std::string qid, label;            // Wikidata item that matched
    std::string description;           // Wikipedia summary extract
    std::string page_url;              // article the description comes from
    std::string source;                // "Wikipedia" / "GameTDB" (who wrote the description)
    int rev = 0;                       // kCacheRev of the code that fetched it: older entries without a description are looked up again
    std::vector<Rating> ratings;       // review scores from Wikidata (P444 + P447)
    std::string esrb, pegi;            // "T", "PEGI 12"
    std::string developer, publisher, genre;
    int year = 0;
};

/* GET callback: true when an HTTP response arrived (status set); false on a network error. */
using Fetcher = std::function<bool(const std::string& url, std::string* body, int* status)>;

/* Wikidata platform item for a library system; "" when unknown. */
std::string platform_qid(const std::string& system);
/* Every platform item accepted for a system (Switch also accepts Switch 2). */
std::vector<std::string> platform_qids(const std::string& system);
/* Titles to look up, best first, deduplicated by normalize(): curated alt_title, the cleaned library title, and the full form of the short
 * ones ("Zelda: The Wind Waker" -> "The Legend of Zelda: The Wind Waker"). */
std::vector<std::string> title_variants(const Query& q);
/* The URL builders (exposed so tests / the fixture recorder use the exact strings). */
std::string url_search(const std::string& text);
std::string url_entities(const std::vector<std::string>& ids);
std::string url_labels(const std::vector<std::string>& ids);
std::string url_summary(const std::string& wiki_title);

enum class ResolveResult { Ok, NotFound, NetworkError };
/* Looks the game up. Network failure -> NetworkError (nothing to cache). NotFound = no confident title+platform match. */
ResolveResult resolve_online(const Query& q, const Fetcher& fetch, Online* out);

/* GameTDB record for a query (id from the serial first, then the title), or nullptr. The caller owns the network side (gametdb::index_for). */
using TdbFinder = std::function<const gametdb::Rec*(const Query&)>;
/* resolve_online() and, for what Wikipedia did not give (no match, or a match without a description), GameTDB's synopsis, developer,
 * publisher, genre and ratings. A NotFound that GameTDB can fill becomes Ok. */
ResolveResult resolve_description(const Query& q, const Fetcher& fetch, const TdbFinder& tdb, Online* out);

/* A factual one-liner built from metadata only, never plot: "A 2001 racing game for the Nintendo 64 by Rare, for 1-4 players."
 * fan_base != "": a ROM hack / mod ("A fan-made modification of Super Smash Bros. for the Nintendo 64."; "*" = base unknown). */
std::string generated_description(const Query& q, const Meta& m, const Online& o, bool nx2 = false, const std::string& fan_base = "");
std::string system_display_name(const std::string& system_short, bool nx2 = false);

constexpr int kCacheRev = 2;   // 2: sitelink redirects accepted, GameTDB fallback, Switch 2, title variants

/* Cache file name (no directory) for a query: "<system>_<normalized title>.json". */
std::string cache_name(const Query& q);
std::string cache_to_json(const Online& o);
bool cache_from_json(const std::string& text, Online* o);
/* Should the cached result be refreshed now? Ok: after 90 days; NotFound: after 7 days; Unknown: always. */
bool cache_stale(const Online& o, int64_t now);

/* ---- presentation ---- */
struct Chip { std::string label, value; };
/* Rating chips in display order (Metacritic, GameRankings, Famitsu, Edge, IGN, others), at most `max`. Offline Famitsu/Edge are merged in. */
std::vector<Chip> rating_chips(const Meta& m, const Online& o, size_t max = 5);
/* "ESRB T" style age badge text from libretro data and/or Wikidata; "" when none. */
std::string age_badge(const Meta& m, const Online& o);
std::string players_text(int n);                 // "1 player" / "1-4 players" / ""
std::string release_text(const Meta& m, const Online& o);   // "November 1999" / "1999" / ""

} // namespace gameinfo
