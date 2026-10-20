#pragma once
/* Pure (no I/O, no SDL, no network) logic for cover art: libretro thumbnail naming,
 * No-Intro / Redump DAT parsing and game-to-name matching. Unit-tested by hub_art_test. */

#include <map>
#include <string>
#include <vector>

namespace hubart_match {

/* libretro-thumbnails rule: the characters  & * / : ` < > ? \ |  become '_'. */
std::string thumb_name(const std::string& nointro_name);

/* Percent-encode UTF-8 for a URL path segment. Unreserved characters plus ( ) ! ' are kept. */
std::string url_encode(const std::string& s);

/* Per-system data. system_short: n64 gba gb gbc gc. Empty string when unsupported. */
std::string thumb_repo(const std::string& system_short);       // "Nintendo_-_Nintendo_64"
std::string dat_url(const std::string& system_short);          // full raw.githubusercontent URL
const char* kind_dir(int kind);                                // 0 Named_Boxarts, 1 Named_Titles, 2 Named_Snaps
std::string thumb_url(const std::string& system_short, int kind, const std::string& nointro_name);

/* One ROM line of a DAT (game-level serial is inherited when the rom has none). */
struct DatRec {
    std::string name;     // game name, e.g. "Mario Kart 64 (USA)"
    std::string serial;   // as in the DAT ("NO7E", "BPEE", "DL-DOL-GW7E-USA"); may be empty
    std::string md5;      // uppercase hex, may be empty
    std::string crc;      // uppercase hex (8), may be empty
};
std::vector<DatRec> parse_dat(const std::string& text);

/* Lowercase, drop (...) and [...] tags, keep only [a-z0-9]. */
std::string normalize_title(const std::string& s);

/* Lower is better: USA 0, Europe 1, World 2, other 3; +10 proto/beta/demo/sample/unl; +1 Rev/alt. */
int release_score(const std::string& name);

struct Query {
    std::string system;   // n64 gba gb gbc gc
    std::string serial;   // header ID, any case
    std::string md5;      // any case, optional
    std::string crc;      // hex, optional
    std::string title;    // header / display title, optional
};
/* Best DAT name for the query; "" if none. *how (optional) is one of md5 crc serial title. */
std::string match_name(const std::vector<DatRec>& dat, const Query& q, std::string* how = nullptr);

/* Flat string->string JSON object (used for names.json and misses.json). */
std::string map_to_json(const std::map<std::string, std::string>& m);
bool map_from_json(const std::string& text, std::map<std::string, std::string>* out);

} // namespace hubart_match
